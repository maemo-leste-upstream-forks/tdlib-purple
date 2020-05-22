#include "td-client.h"
#include "chat-info.h"
#include "config.h"
#include "format.h"
#include <unistd.h>
#include <stdlib.h>

static char *_(const char *s) { return const_cast<char *>(s); }

enum {
    // Typing notifications seems to be resent every 5-6 seconds, so 10s timeout hould be appropriate
    REMOTE_TYPING_NOTICE_TIMEOUT = 10,
    FILE_DOWNLOAD_PRIORITY       = 1
};

static bool isChatInContactList(const td::td_api::chat &chat, const td::td_api::user *privateChatUser)
{
    return (chat.chat_list_ != nullptr) || (privateChatUser && privateChatUser->is_contact_);
}

PurpleTdClient::PurpleTdClient(PurpleAccount *acct, ITransceiverBackend *testBackend)
:   m_transceiver(this, acct, &PurpleTdClient::processUpdate, testBackend),
    m_data(acct)
{
    m_account = acct;
}

PurpleTdClient::~PurpleTdClient()
{
}

void PurpleTdClient::setLogLevel(int level)
{
    // Why not just call setLogVerbosityLevel? No idea!
    td::Client::execute({0, td::td_api::make_object<td::td_api::setLogVerbosityLevel>(level)});
}

void PurpleTdClient::processUpdate(td::td_api::Object &update)
{
    purple_debug_misc(config::pluginId, "Incoming update\n");

    switch (update.get_id()) {
    case td::td_api::updateAuthorizationState::ID: {
        auto &update_authorization_state = static_cast<td::td_api::updateAuthorizationState &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: authorization state\n");
        if (update_authorization_state.authorization_state_) {
            m_lastAuthState = update_authorization_state.authorization_state_->get_id();
            processAuthorizationState(*update_authorization_state.authorization_state_);
        }
        break;
    }

    case td::td_api::updateConnectionState::ID: {
        auto &connectionUpdate = static_cast<td::td_api::updateConnectionState &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: connection state\n");
        if (connectionUpdate.state_) {
            if (connectionUpdate.state_->get_id() == td::td_api::connectionStateReady::ID)
                connectionReady();
            else if (connectionUpdate.state_->get_id() == td::td_api::connectionStateConnecting::ID)
                setPurpleConnectionInProgress();
            else if (connectionUpdate.state_->get_id() == td::td_api::connectionStateUpdating::ID)
                setPurpleConnectionUpdating();
        }
        break;
    }

    case td::td_api::updateUser::ID: {
        auto &userUpdate = static_cast<td::td_api::updateUser &>(update);
        updateUser(std::move(userUpdate.user_));
        break;
    }

    case td::td_api::updateNewChat::ID: {
        auto &newChat = static_cast<td::td_api::updateNewChat &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: new chat\n");
        addChat(std::move(newChat.chat_));
        break;
    }

    case td::td_api::updateNewMessage::ID: {
        auto &newMessageUpdate = static_cast<td::td_api::updateNewMessage &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: new message\n");
        if (newMessageUpdate.message_)
            onIncomingMessage(std::move(newMessageUpdate.message_));
        else
            purple_debug_warning(config::pluginId, "Received null new message\n");
        break;
    }

    case td::td_api::updateUserStatus::ID: {
        auto &updateStatus = static_cast<td::td_api::updateUserStatus &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: user status\n");
        if (updateStatus.status_)
            updateUserStatus(updateStatus.user_id_, std::move(updateStatus.status_));
        break;
    }

    case td::td_api::updateUserChatAction::ID: {
        auto &updateChatAction = static_cast<td::td_api::updateUserChatAction &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: chat action %d\n",
            updateChatAction.action_ ? updateChatAction.action_->get_id() : 0);
        handleUserChatAction(updateChatAction);
        break;
    }

    case td::td_api::updateBasicGroup::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateBasicGroup &>(update);
        updateGroup(std::move(groupUpdate.basic_group_));
        break;
    }

    case td::td_api::updateSupergroup::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateSupergroup &>(update);
        updateSupergroup(std::move(groupUpdate.supergroup_));
        break;
    }

    case td::td_api::updateMessageSendSucceeded::ID: {
        auto &sendSucceeded = static_cast<const td::td_api::updateMessageSendSucceeded &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " send succeeded\n",
                          sendSucceeded.old_message_id_);
        removeTempFile(sendSucceeded.old_message_id_);
        break;
    }

    case td::td_api::updateMessageSendFailed::ID: {
        auto &sendFailed = static_cast<const td::td_api::updateMessageSendFailed &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " send failed\n",
                          sendFailed.old_message_id_);
        removeTempFile(sendFailed.old_message_id_);
        break;
    }

    case td::td_api::updateChatChatList::ID: {
        auto &chatListUpdate = static_cast<td::td_api::updateChatChatList &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: update chat list for chat %" G_GINT64_FORMAT "\n",
                          chatListUpdate.chat_id_);
        m_data.updateChatChatList(chatListUpdate.chat_id_, std::move(chatListUpdate.chat_list_));
        updateChat(m_data.getChat(chatListUpdate.chat_id_));
        break;
    }

    case td::td_api::updateChatTitle::ID: {
        auto &chatTitleUpdate = static_cast<td::td_api::updateChatTitle &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: update chat title for chat %" G_GINT64_FORMAT "\n",
                          chatTitleUpdate.chat_id_);
        m_data.updateChatTitle(chatTitleUpdate.chat_id_, chatTitleUpdate.title_);
        updateChat(m_data.getChat(chatTitleUpdate.chat_id_));
        break;
    }

    default:
        purple_debug_misc(config::pluginId, "Incoming update: ignorig ID=%d\n", update.get_id());
        break;
    }
}

void PurpleTdClient::processAuthorizationState(td::td_api::AuthorizationState &authState)
{
    switch (authState.get_id()) {
    case td::td_api::authorizationStateWaitEncryptionKey::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: encriytion key requested\n");
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::checkDatabaseEncryptionKey>(""),
                                &PurpleTdClient::authResponse);
        break;

    case td::td_api::authorizationStateWaitTdlibParameters::ID: 
        purple_debug_misc(config::pluginId, "Authorization state update: TDLib parameters requested\n");
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::disableProxy>(), nullptr);
        if (addProxy()) {
            m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getProxies>(),
                                    &PurpleTdClient::getProxiesResponse);
            sendTdlibParameters();
        }
        break;

    case td::td_api::authorizationStateWaitPhoneNumber::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: phone number requested\n");
        sendPhoneNumber();
        break;

    case td::td_api::authorizationStateWaitCode::ID: {
        auto &codeState = static_cast<td::td_api::authorizationStateWaitCode &>(authState);
        purple_debug_misc(config::pluginId, "Authorization state update: authentication code requested\n");
        requestAuthCode(codeState.code_info_.get());
        break;
    }

    case td::td_api::authorizationStateWaitRegistration::ID: {
        purple_debug_misc(config::pluginId, "Authorization state update: new user registration\n");
        registerUser();
        break;
    }

    case td::td_api::authorizationStateReady::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: ready\n");
        if (m_connectionReady)
            onLoggedIn();
        break;
    }
}

bool PurpleTdClient::addProxy()
{
    PurpleProxyInfo *purpleProxy = purple_proxy_get_setup(m_account);
    PurpleProxyType  proxyType   = purpleProxy ? purple_proxy_info_get_type(purpleProxy) : PURPLE_PROXY_NONE;
    const char *     username    = purpleProxy ? purple_proxy_info_get_username(purpleProxy) : "";
    const char *     password    = purpleProxy ? purple_proxy_info_get_password(purpleProxy) : "";
    const char *     host        = purpleProxy ? purple_proxy_info_get_host(purpleProxy) : "";
    int              port        = purpleProxy ? purple_proxy_info_get_port(purpleProxy) : 0;
    if (username == NULL) username = "";
    if (password == NULL) password = "";
    if (host == NULL) host = "";
    std::string errorMessage;

    td::td_api::object_ptr<td::td_api::ProxyType> tdProxyType;
    switch (proxyType) {
    case PURPLE_PROXY_NONE:
        tdProxyType = nullptr;
        break;
    case PURPLE_PROXY_SOCKS5:
        tdProxyType = td::td_api::make_object<td::td_api::proxyTypeSocks5>(username, password);
        break;
    case PURPLE_PROXY_HTTP:
        tdProxyType = td::td_api::make_object<td::td_api::proxyTypeHttp>(username, password, true);
        break;
    default:
        errorMessage = formatMessage(_("Proxy type {} is not supported"), proxyTypeToString(proxyType));
        break;
    }

    if (!errorMessage.empty()) {
        purple_connection_error(purple_account_get_connection(m_account), errorMessage.c_str());
        return false;
    } else if (tdProxyType) {
        auto addProxy = td::td_api::make_object<td::td_api::addProxy>();
        addProxy->server_ = host;
        addProxy->port_ = port;
        addProxy->enable_ = true;
        addProxy->type_ = std::move(tdProxyType);
        m_transceiver.sendQuery(std::move(addProxy), &PurpleTdClient::addProxyResponse);
        m_isProxyAdded = true;
    }

    return true;
}

static std::string getDisplayedError(const td::td_api::object_ptr<td::td_api::Object> &object)
{
    if (!object)
        return _("No response received");
    else if (object->get_id() == td::td_api::error::ID) {
        const td::td_api::error &error = static_cast<const td::td_api::error &>(*object);
        return formatMessage("code {} ({})", {std::to_string(error.code_), error.message_});
    } else
        return _("Unexpected response");
}

void PurpleTdClient::addProxyResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::proxy::ID)) {
        m_addedProxy = td::move_tl_object_as<td::td_api::proxy>(object);
        if (m_proxies)
            removeOldProxies();
    } else {
        std::string message = formatMessage(_("Could not set proxy: {}"), getDisplayedError(object));
        purple_connection_error(purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::getProxiesResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::proxies::ID)) {
        m_proxies = td::move_tl_object_as<td::td_api::proxies>(object);
        if (!m_isProxyAdded || m_addedProxy)
            removeOldProxies();
    } else {
        std::string message = formatMessage(_("Could not get proxies: {}"), getDisplayedError(object));
        purple_connection_error(purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::removeOldProxies()
{
    for (const td::td_api::object_ptr<td::td_api::proxy> &proxy: m_proxies->proxies_)
        if (proxy && (!m_addedProxy || (proxy->id_ != m_addedProxy->id_)))
            m_transceiver.sendQuery(td::td_api::make_object<td::td_api::removeProxy>(proxy->id_), nullptr);
}

void PurpleTdClient::sendTdlibParameters()
{
    auto parameters = td::td_api::make_object<td::td_api::tdlibParameters>();
    const char *username = purple_account_get_username(m_account);
    parameters->database_directory_ = std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
                                      config::configSubdir + G_DIR_SEPARATOR_S + username;
    purple_debug_misc(config::pluginId, "Account %s using database directory %s\n",
                      username, parameters->database_directory_.c_str());
    parameters->use_message_database_ = true;
    parameters->use_secret_chats_ = true;
    parameters->api_id_ = 94575;
    parameters->api_hash_ = "a3406de8d171bb422bb6ddf3bbd800e2";
    parameters->system_language_code_ = "en";
    parameters->device_model_ = "Desktop";
    parameters->system_version_ = "Unknown";
    parameters->application_version_ = "1.0";
    parameters->enable_storage_optimizer_ = true;
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::setTdlibParameters>(std::move(parameters)),
                            &PurpleTdClient::authResponse);
}

void PurpleTdClient::sendPhoneNumber()
{
    const char *number = purple_account_get_username(m_account);
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(number, nullptr),
                            &PurpleTdClient::authResponse);
}

static std::string getAuthCodeDesc(const td::td_api::AuthenticationCodeType &codeType)
{
    switch (codeType.get_id()) {
    case td::td_api::authenticationCodeTypeTelegramMessage::ID:
        return formatMessage(_("Telegram message (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeTelegramMessage &>(codeType).length_);
    case td::td_api::authenticationCodeTypeSms::ID:
        return formatMessage(_("SMS (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeSms &>(codeType).length_);
    case td::td_api::authenticationCodeTypeCall::ID:
        return formatMessage(_("Phone call (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeCall &>(codeType).length_);
    case td::td_api::authenticationCodeTypeFlashCall::ID:
        return formatMessage(_("Poor man's phone call (pattern: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeFlashCall &>(codeType).pattern_);
    default:
        return "Pigeon post";
    }
}

void PurpleTdClient::requestAuthCode(const td::td_api::authenticationCodeInfo *codeInfo)
{
    std::string message = _("Enter authentication code") + std::string("\n");

    if (codeInfo) {
        if (codeInfo->type_)
            message += formatMessage(_("Code sent via: {}"), getAuthCodeDesc(*codeInfo->type_)) + "\n";
        if (codeInfo->next_type_)
            message += formatMessage(_("Next code will be: {}"), getAuthCodeDesc(*codeInfo->next_type_)) + "\n";
    }

    if (!purple_request_input (purple_account_get_connection(m_account),
                               _("Login code"),
                               message.c_str(),
                               NULL, // secondary message
                               NULL, // default value
                               FALSE, // multiline input
                               FALSE, // masked input
                               _("the code"),
                               _("OK"), G_CALLBACK(requestCodeEntered),
                               _("Cancel"), G_CALLBACK(requestCodeCancelled),
                               m_account,
                               NULL, // buddy
                               NULL, // conversation
                               this))
    {
        purple_connection_set_state (purple_account_get_connection(m_account), PURPLE_CONNECTED);
        PurpleConversation *conv = purple_conversation_new (PURPLE_CONV_TYPE_IM, m_account, "Telegram");
        purple_conversation_write (conv, "Telegram",
            "Authentication code needs to be entered but this libpurple won't cooperate",
            (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM), 0);
    }
}

void PurpleTdClient::registerUser()
{
    std::string firstName, lastName;
    getNamesFromAlias(purple_account_get_alias(m_account), firstName, lastName);

    if (firstName.empty() && lastName.empty())
        purple_connection_error(purple_account_get_connection(m_account),
                                _("Account alias (your name) must be set to register new user"));
    else
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::registerUser>(firstName, lastName),
                                &PurpleTdClient::authResponse);
}

void PurpleTdClient::requestCodeEntered(PurpleTdClient *self, const gchar *code)
{
    purple_debug_misc(config::pluginId, "Authentication code entered: '%s'\n", code);
    self->m_transceiver.sendQuery(td::td_api::make_object<td::td_api::checkAuthenticationCode>(code),
                                  &PurpleTdClient::authResponse);
}

void PurpleTdClient::requestCodeCancelled(PurpleTdClient *self)
{
    purple_connection_error(purple_account_get_connection(self->m_account),
                            _("Authentication code required"));
}

void PurpleTdClient::authResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::ok::ID))
        purple_debug_misc(config::pluginId, "Authentication success on query %lu\n", (unsigned long)requestId);
    else
        notifyAuthError(object);
}

void PurpleTdClient::notifyAuthError(const td::td_api::object_ptr<td::td_api::Object> &response)
{
    std::string message;
    switch (m_lastAuthState) {
    case td::td_api::authorizationStateWaitEncryptionKey::ID:
        message = _("Error applying database encryption key: {}");
        break;
    case td::td_api::authorizationStateWaitPhoneNumber::ID:
        message = _("Authentication error after sending phone number: {}");
        break;
    default:
        message = _("Authentication error: {}");
    }

    message = formatMessage(message.c_str(), getDisplayedError(response));

    purple_connection_error(purple_account_get_connection(m_account), message.c_str());
}

void PurpleTdClient::connectionReady()
{
    purple_debug_misc(config::pluginId, "Connection ready\n");
    m_connectionReady = true;
    if (m_lastAuthState == td::td_api::authorizationStateReady::ID)
        onLoggedIn();
}

void PurpleTdClient::setPurpleConnectionInProgress()
{
    purple_debug_misc(config::pluginId, "Connection in progress\n");
    m_connectionReady = false;
    PurpleConnection *gc = purple_account_get_connection(m_account);

    if (PURPLE_CONNECTION_IS_CONNECTED(gc))
        purple_blist_remove_account(m_account);
    purple_connection_set_state (gc, PURPLE_CONNECTING);
    purple_connection_update_progress(gc, "Connecting", 1, 3);
}

void PurpleTdClient::setPurpleConnectionUpdating()
{
    purple_debug_misc(config::pluginId, "Updating account status\n");
    m_connectionReady = false;
    PurpleConnection *gc = purple_account_get_connection(m_account);

    purple_connection_update_progress(gc, "Updating status", 2, 3);
}

void PurpleTdClient::onLoggedIn()
{
    // This query ensures an updateUser for every contact
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getContacts>(),
                            &PurpleTdClient::getContactsResponse);
}

void PurpleTdClient::getContactsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    purple_debug_misc(config::pluginId, "getContacts response to request %" G_GUINT64_FORMAT "\n", requestId);
    if (object && (object->get_id() == td::td_api::users::ID)) {
        td::td_api::object_ptr<td::td_api::users> users = td::move_tl_object_as<td::td_api::users>(object);
        m_data.setContacts(users->user_ids_);
        // td::td_api::chats response will be preceded by a string of updateNewChat for all chats
        // apparently even if td::td_api::getChats has limit_ of like 1
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getChats>(
                                    nullptr, std::numeric_limits<std::int64_t>::max(), 0, 200),
                                &PurpleTdClient::getChatsResponse);
    } else
        notifyAuthError(object);
}

void PurpleTdClient::getChatsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    purple_debug_misc(config::pluginId, "getChats response to request %" G_GUINT64_FORMAT "\n", requestId);
    if (object && (object->get_id() == td::td_api::chats::ID)) {
        td::td_api::object_ptr<td::td_api::chats> chats = td::move_tl_object_as<td::td_api::chats>(object);
        m_data.getContactsWithNoChat(m_usersForNewPrivateChats);
        requestMissingPrivateChats();
    } else
        notifyAuthError(object);
}

void PurpleTdClient::requestMissingPrivateChats()
{
    if (m_usersForNewPrivateChats.empty()) {
        purple_debug_misc(config::pluginId, "Login sequence complete\n");
        updatePurpleChatListAndReportConnected();
    } else {
        int32_t userId = m_usersForNewPrivateChats.back();
        m_usersForNewPrivateChats.pop_back();
        purple_debug_misc(config::pluginId, "Requesting private chat for user id %d\n", (int)userId);
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(userId, false);
        m_transceiver.sendQuery(std::move(createChat), &PurpleTdClient::loginCreatePrivateChatResponse);
    }
}

void PurpleTdClient::loginCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object->get_id() == td::td_api::chat::ID) {
        td::td_api::object_ptr<td::td_api::chat> chat = td::move_tl_object_as<td::td_api::chat>(object);
        purple_debug_misc(config::pluginId, "Requested private chat received: id %" G_GINT64_FORMAT "\n",
                          chat->id_);
        // Here the "new" chat already exists in AccountData because there has just been
        // updateNewChat about this same chat. But do addChat anyway, just in case.
        m_data.addChat(std::move(chat));
    } else
        purple_debug_misc(config::pluginId, "Failed to get requested private chat\n");
    requestMissingPrivateChats();
}

void PurpleTdClient::requestBasicGroupMembers(int32_t groupId)
{
    if (!m_data.isBasicGroupInfoRequested(groupId)) {
        m_data.setBasicGroupInfoRequested(groupId);
        uint64_t requestId = m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getBasicGroupFullInfo>(groupId),
                                                     &PurpleTdClient::groupInfoResponse);
        m_data.addPendingRequest<GroupInfoRequest>(requestId, groupId);
    }
}

// TODO process messageChatAddMembers and messageChatDeleteMember
// TODO process messageChatUpgradeTo and messageChatUpgradeFrom
void PurpleTdClient::groupInfoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupInfoRequest> request = m_data.getPendingRequest<GroupInfoRequest>(requestId);

    if (request && object && (object->get_id() == td::td_api::basicGroupFullInfo::ID)) {
        td::td_api::object_ptr<td::td_api::basicGroupFullInfo> groupInfo =
            td::move_tl_object_as<td::td_api::basicGroupFullInfo>(object);
        const td::td_api::chat *chat = m_data.getBasicGroupChatByGroup(request->groupId);

        if (chat) {
            PurpleConvChat *purpleChat = findChatConversation(m_account, *chat);
            if (purpleChat)
                setChatMembers(purpleChat, *groupInfo, m_data);
        }

        m_data.updateBasicGroupInfo(request->groupId, std::move(groupInfo));
    }
}

void PurpleTdClient::updatePurpleChatListAndReportConnected()
{
    purple_connection_set_state (purple_account_get_connection(m_account), PURPLE_CONNECTED);

    std::vector<const td::td_api::chat *> chats;
    m_data.getChats(chats);

    for (const td::td_api::chat *chat: chats) {
        updateChat(chat);

        const td::td_api::user *user = m_data.getUserByPrivateChat(*chat);
        if (user && isChatInContactList(*chat, user)) {
            std::string userName = getPurpleBuddyName(*user);
            purple_prpl_got_user_status(m_account, userName.c_str(),
                                        getPurpleStatusId(*user->status_), NULL);
        }
    }

    // Here we could remove buddies for which no private chat exists, meaning they have been remove
    // from the contact list perhaps in another client

    const td::td_api::user *selfInfo = m_data.getUserByPhone(purple_account_get_username(m_account));
    if (selfInfo != nullptr) {
        std::string alias = selfInfo->first_name_ + " " + selfInfo->last_name_;
        purple_debug_misc(config::pluginId, "Setting own alias to '%s'\n", alias.c_str());
        purple_account_set_alias(m_account, alias.c_str());
    } else
        purple_debug_warning(config::pluginId, "Did not receive user information for self (%s) at login\n",
            purple_account_get_username(m_account));

    purple_blist_add_account(m_account);
}

void PurpleTdClient::showTextMessage(const td::td_api::chat &chat, const TgMessageInfo &message,
                                     const td::td_api::messageText &text)
{
    if (text.text_)
        showMessageText(m_data, chat, message, text.text_->text_.c_str(), NULL);
}

static const td::td_api::file *selectPhotoSize(const td::td_api::messagePhoto &photo)
{
    const td::td_api::photoSize *selectedSize = nullptr;
    if (photo.photo_)
        for (const auto &newSize: photo.photo_->sizes_)
            if (newSize && newSize->photo_ && (!selectedSize || (newSize->width_ > selectedSize->width_)))
                selectedSize = newSize.get();

    if (selectedSize)
        purple_debug_misc(config::pluginId, "Selected size %dx%d for photo\n",
                          (int)selectedSize->width_, (int)selectedSize->height_);
    else
        purple_debug_warning(config::pluginId, "No file found for a photo\n");

    return selectedSize ? selectedSize->photo_.get() : nullptr;
}

static std::string getSenderDisplayName(const td::td_api::chat &chat, const TgMessageInfo &message,
                                        PurpleAccount *account)
{
    if (message.outgoing)
        return purple_account_get_alias(account);
    else if (isPrivateChat(chat))
        return chat.title_;
    else
        return message.sender;
}

static std::string makeNoticeWithSender(const td::td_api::chat &chat, const TgMessageInfo &message,
                                        const char *noticeText, PurpleAccount *account)
{
    std::string prefix = getSenderDisplayName(chat, message, account);
    if (!prefix.empty())
        prefix += ": ";
    return prefix + noticeText;
}

void PurpleTdClient::showPhotoMessage(const td::td_api::chat &chat, const TgMessageInfo &message,
                                      const td::td_api::messagePhoto &photo)
{
    const td::td_api::file *file = selectPhotoSize(photo);
    std::string             notice;

    if (!file)
        notice = makeNoticeWithSender(chat, message, _("Faulty image"), m_account);
    else if (file->local_ && file->local_->is_downloading_completed_)
        notice.clear();
    else
        notice = makeNoticeWithSender(chat, message, _("Downloading image"), m_account);

    const char *caption = photo.caption_ ? photo.caption_->text_.c_str() : NULL;

    if (!notice.empty())
        showMessageText(m_data, chat, message, caption, notice.c_str());

    if (file)
        showImage(chat, message, *file, caption);
}

void PurpleTdClient::requestDownload(int32_t fileId, int64_t chatId, const TgMessageInfo &message,
                                     td::td_api::object_ptr<td::td_api::file> thumbnail,
                                     TdTransceiver::ResponseCb responseCb)
{
    td::td_api::object_ptr<td::td_api::downloadFile> downloadReq =
        td::td_api::make_object<td::td_api::downloadFile>();
    downloadReq->file_id_     = fileId;
    downloadReq->priority_    = FILE_DOWNLOAD_PRIORITY;
    downloadReq->offset_      = 0;
    downloadReq->limit_       = 0;
    downloadReq->synchronous_ = true;

    uint64_t requestId = m_transceiver.sendQuery(std::move(downloadReq), responseCb);
    m_data.addPendingRequest<DownloadRequest>(requestId, chatId, message, thumbnail.release());
}

void PurpleTdClient::showImage(const td::td_api::chat &chat, const TgMessageInfo &message,
                               const td::td_api::file &file, const char *caption)
{
    if (file.local_ && file.local_->is_downloading_completed_)
        showDownloadedImage(chat.id_, message, file.local_->path_, caption);
    else {
        purple_debug_misc(config::pluginId, "Downloading image (file id %d)\n", (int)file.id_);
        requestDownload(file.id_, chat.id_, message, nullptr, &PurpleTdClient::imageDownloadResponse);
    }
}

static std::string getDownloadPath(const td::td_api::Object *object)
{
    if (!object)
        purple_debug_misc(config::pluginId, "No response after downloading file\n");
    else if (object->get_id() == td::td_api::file::ID) {
        const td::td_api::file &file = static_cast<const td::td_api::file &>(*object);
        if (!file.local_)
            purple_debug_misc(config::pluginId, "No local file info after downloading\n");
        else if (!file.local_->is_downloading_completed_)
            purple_debug_misc(config::pluginId, "File not completely downloaded\n");
        else
            return file.local_->path_;
    } else
        purple_debug_misc(config::pluginId, "Unexpected response to downloading file: id %d\n",
                          (int)object->get_id());

    return "";
}

void PurpleTdClient::imageDownloadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::string                      path    = getDownloadPath(object.get());
    std::unique_ptr<DownloadRequest> request = m_data.getPendingRequest<DownloadRequest>(requestId);

    if (request && !path.empty()) {
        purple_debug_misc(config::pluginId, "Image downloaded, path: %s\n", path.c_str());
        // For image that needed downloading, caption was shown as soon as message was received
        showDownloadedImage(request->chatId, request->message, path, NULL);
    }
}

void PurpleTdClient::showDownloadedImage(int64_t chatId, const TgMessageInfo &message,
                                         const std::string &filePath, const char *caption)
{
    const td::td_api::chat *chat = m_data.getChat(chatId);
    if (chat) {
        std::string  text;
        std::string notice;
        gchar       *data   = NULL;
        size_t       len    = 0;

        if (g_file_get_contents (filePath.c_str(), &data, &len, NULL)) {
            int id = purple_imgstore_add_with_id (data, len, NULL);
            text = "\n<img id=\"" + std::to_string(id) + "\">";
        } else if (filePath.find('"') == std::string::npos)
            text = "<img src=\"file://" + filePath + "\">";
        else
            notice = makeNoticeWithSender(*chat, message,  "Cannot show photo: file path contains quotes", m_account);

        if (caption && *caption) {
            if (!text.empty())
                text += "\n";
            text += caption;
        }

        showMessageText(m_data, *chat, message, text.empty() ? NULL : text.c_str(),
                        notice.empty() ? NULL : notice.c_str(), PURPLE_MESSAGE_IMAGES);
    }
}

void PurpleTdClient::showDocument(const td::td_api::chat &chat, const TgMessageInfo &message,
                                  const td::td_api::messageDocument &document)
{
    std::string description = makeNoticeWithSender(chat, message, "Sent a file", m_account);
    if (document.document_)
        description = description + ": " + document.document_->file_name_ + " [" +
        document.document_->mime_type_ + "]";

    showMessageText(m_data, chat, message,
                    document.caption_ ? document.caption_->text_.c_str() : NULL,
                    description.c_str());
}

void PurpleTdClient::showVideo(const td::td_api::chat &chat, const TgMessageInfo &message,
                               const td::td_api::messageVideo &video)
{
    std::string description = makeNoticeWithSender(chat, message, "Sent a video", m_account);
    if (video.video_)
        description = description + ": " + video.video_->file_name_ + " [" +
        std::to_string(video.video_->width_) + "x" + std::to_string(video.video_->height_) + ", " +
        std::to_string(video.video_->duration_) + "s]";

    showMessageText(m_data, chat, message, video.caption_ ? video.caption_->text_.c_str() : NULL,
                    description.c_str());
}

void PurpleTdClient::showSticker(const td::td_api::chat &chat, const TgMessageInfo &message,
                                 td::td_api::messageSticker &stickerContent)
{
    if (!stickerContent.sticker_) return;
    td::td_api::sticker &sticker = *stickerContent.sticker_;

    if (sticker.sticker_) {
        auto thumbnail = sticker.thumbnail_ ? std::move(sticker.thumbnail_->photo_) : nullptr;

        if (sticker.sticker_->local_ && sticker.sticker_->local_->is_downloading_completed_)
            showDownloadedSticker(chat.id_, message, sticker.sticker_->local_->path_,
                                  std::move(thumbnail));
        else {
            purple_debug_misc(config::pluginId, "Downloading sticker (file id %d)\n", (int)sticker.sticker_->id_);
            requestDownload(sticker.sticker_->id_, chat.id_, message, std::move(thumbnail),
                            &PurpleTdClient::stickerDownloadResponse);
        }
    }
}

static bool isTgs(const std::string &path)
{
    size_t dot = path.rfind('.');
    if (dot != std::string::npos)
        return !strcmp(path.c_str() + dot + 1, "tgs");

    return false;
}


void PurpleTdClient::stickerDownloadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::string                      path    = getDownloadPath(object.get());
    std::unique_ptr<DownloadRequest> request = m_data.getPendingRequest<DownloadRequest>(requestId);

    if (request && !path.empty())
        showDownloadedSticker(request->chatId, request->message, path, std::move(request->thumbnail));
}

void PurpleTdClient::showDownloadedSticker(int64_t chatId, const TgMessageInfo &message,
                                           const std::string &filePath,
                                           td::td_api::object_ptr<td::td_api::file> thumbnail)
{
    if (isTgs(filePath) && thumbnail) {
        if (thumbnail->local_ && thumbnail->local_->is_downloading_completed_)
            showDownloadedInlineFile(chatId, message, thumbnail->local_->path_, "Sticker");
        else
            requestDownload(thumbnail->id_, chatId, message, nullptr,
                            &PurpleTdClient::stickerDownloadResponse);
    } else
        showDownloadedInlineFile(chatId, message, filePath, "Sticker");
}


void PurpleTdClient::showInlineFile(const td::td_api::chat &chat, const TgMessageInfo &message,
                                    const td::td_api::file &file)
{
    if (file.local_ && file.local_->is_downloading_completed_)
        showDownloadedInlineFile(chat.id_, message, file.local_->path_, "Sent file");
    else {
        purple_debug_misc(config::pluginId, "Downloading file (id %d)\n", (int)file.id_);
        requestDownload(file.id_, chat.id_, message, nullptr,
                        &PurpleTdClient::fileDownloadResponse);
    }
}

void PurpleTdClient::fileDownloadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::string                      path    = getDownloadPath(object.get());
    std::unique_ptr<DownloadRequest> request = m_data.getPendingRequest<DownloadRequest>(requestId);

    if (request && !path.empty()) {
        purple_debug_misc(config::pluginId, "File downloaded, path: %s\n", path.c_str());
        showDownloadedInlineFile(request->chatId, request->message, path, "Sent file");
    }
}

void PurpleTdClient::showDownloadedInlineFile(int64_t chatId, const TgMessageInfo &message,
                                              const std::string &filePath, const char *label)
{
    const td::td_api::chat *chat = m_data.getChat(chatId);
    if (chat) {
        if (filePath.find('"') != std::string::npos) {
            std::string notice = makeNoticeWithSender(*chat, message, "Cannot show file: path contains quotes", m_account);
            showMessageText(m_data, *chat, message, NULL, notice.c_str());
        } else {
            std::string text = "<a href=\"file://" + filePath + "\">" + label + "</a>";
            showMessageText(m_data, *chat, message, text.c_str(), NULL);
        }
    }
}

void PurpleTdClient::showMessage(const td::td_api::chat &chat, int64_t messageId)
{
    td::td_api::message *message = m_data.findMessage(messageId);
    if (!message || !message->content_)
        return;
    purple_debug_misc(config::pluginId, "Displaying message %" G_GINT64_FORMAT "\n", messageId);

    TgMessageInfo messageInfo;
    messageInfo.sender           = getSenderPurpleName(chat, *message, m_data);
    messageInfo.timestamp        = message->date_;
    messageInfo.outgoing         = message->is_outgoing_;
    messageInfo.repliedMessageId = message->reply_to_message_id_;

    if (message->forward_info_)
        messageInfo.forwardedFrom = getForwardSource(*message->forward_info_, m_data);

    if (message->ttl_ != 0) {
        const char *text   = _("Received self-destructing message, not displayed due to lack of support");
        std::string notice = makeNoticeWithSender(chat, messageInfo, text, m_account);
        showMessageText(m_data, chat, messageInfo, NULL, notice.c_str());
        return;
    }

    switch (message->content_->get_id()) {
        case td::td_api::messageText::ID:
            showTextMessage(chat, messageInfo, static_cast<const td::td_api::messageText &>(*message->content_));
            break;
        case td::td_api::messagePhoto::ID:
            showPhotoMessage(chat, messageInfo, static_cast<const td::td_api::messagePhoto &>(*message->content_));
            break;
        case td::td_api::messageDocument::ID:
            showDocument(chat, messageInfo, static_cast<const td::td_api::messageDocument &>(*message->content_));
            break;
        case td::td_api::messageVideo::ID:
            showVideo(chat, messageInfo, static_cast<const td::td_api::messageVideo &>(*message->content_));
            break;
        case td::td_api::messageSticker::ID:
            showSticker(chat, messageInfo, static_cast<td::td_api::messageSticker &>(*message->content_));
            break;
        case td::td_api::messageChatChangeTitle::ID: {
            const auto &titleChange = static_cast<const td::td_api::messageChatChangeTitle &>(*message->content_);
            std::string notice = formatMessage(_("{} changed group name to {}"),
                                               {getSenderDisplayName(chat, messageInfo, m_account),
                                                titleChange.title_});
            showMessageText(m_data, chat, messageInfo, NULL, notice.c_str());
            break;
        }
        default: {
            std::string notice = formatMessage(_("Received unsupported message type {}"),
                                               messageTypeToString(*message->content_));
            notice = makeNoticeWithSender(chat, messageInfo, notice.c_str(), m_account);
            showMessageText(m_data, chat, messageInfo, NULL, notice.c_str());
        }
    }
}

void PurpleTdClient::onIncomingMessage(td::td_api::object_ptr<td::td_api::message> message)
{
    if (!message)
        return;

    const td::td_api::chat *chat = m_data.getChat(message->chat_id_);
    if (!chat) {
        purple_debug_warning(config::pluginId, "Received message with unknown chat id %" G_GINT64_FORMAT "\n",
                            message->chat_id_);
        return;
    }

    td::td_api::object_ptr<td::td_api::viewMessages> viewMessagesReq = td::td_api::make_object<td::td_api::viewMessages>();
    viewMessagesReq->chat_id_ = chat->id_;
    viewMessagesReq->force_read_ = true; // no idea what "closed chats" are at this point
    viewMessagesReq->message_ids_.push_back(message->id_);
    m_transceiver.sendQuery(std::move(viewMessagesReq), nullptr);

    int64_t messageId      = message->id_;
    int64_t replyMessageId = message->reply_to_message_id_;
    m_data.saveMessage(std::move(message));

    if (replyMessageId && !m_data.findMessage(replyMessageId)) {
        purple_debug_misc(config::pluginId, "Fetching message %" G_GINT64_FORMAT " which message %" G_GINT64_FORMAT " replies to\n",
                          replyMessageId, messageId);
        td::td_api::object_ptr<td::td_api::getMessage> getMessageReq = td::td_api::make_object<td::td_api::getMessage>();
        getMessageReq->chat_id_    = chat->id_;
        getMessageReq->message_id_ = replyMessageId;
        uint64_t requestId = m_transceiver.sendQueryWithTimeout(std::move(getMessageReq),
                                                                &PurpleTdClient::findMessageResponse, 1);
        m_data.addPendingRequest<PendingMessage>(requestId, messageId, chat->id_);
    } else
        showMessage(*chat, messageId);
}

void PurpleTdClient::findMessageResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<PendingMessage> messageInfo = m_data.getPendingRequest<PendingMessage>(requestId);
    if (!messageInfo) return;

    if (object && (object->get_id() == td::td_api::message::ID))
        m_data.saveMessage(td::move_tl_object_as<td::td_api::message>(object));
    else
        purple_debug_misc(config::pluginId, "Failed to fetch reply source for message %" G_GINT64_FORMAT "\n",
                          messageInfo->messageId);

    const td::td_api::chat *chat = m_data.getChat(messageInfo->chatId);
    if (chat)
        showMessage(*chat, messageInfo->messageId);
}

int PurpleTdClient::sendMessage(const char *buddyName, const char *message)
{
    int64_t chatId = getPrivateChatIdByPurpleName(buddyName, m_data, "send message");
    transmitMessage(chatId, message, m_transceiver, m_data, &PurpleTdClient::sendMessageResponse);

    // Message shall not be echoed: tdlib will shortly present it as a new message and it will be displayed then
    return 0;
}

void PurpleTdClient::updateUserStatus(uint32_t userId, td::td_api::object_ptr<td::td_api::UserStatus> status)
{
    const td::td_api::user *user = m_data.getUser(userId);
    if (user) {
        std::string userName = getPurpleBuddyName(*user);
        purple_prpl_got_user_status(m_account, userName.c_str(), getPurpleStatusId(*status), NULL);
        m_data.setUserStatus(userId, std::move(status));
    }
}

void PurpleTdClient::updateUser(td::td_api::object_ptr<td::td_api::user> userInfo)
{
    if (!userInfo) {
        purple_debug_warning(config::pluginId, "updateUser with null user info\n");
        return;
    }

    int32_t userId = userInfo->id_;
    purple_debug_misc(config::pluginId, "Update user: %d '%s' '%s'\n", (int)userId,
                      userInfo->first_name_.c_str(), userInfo->last_name_.c_str());

    m_data.updateUser(std::move(userInfo));

    // For chats, find_chat doesn't work if account is not yet connected, so just in case, don't
    // user find_buddy either
    if (purple_account_is_connected(m_account)) {
        const td::td_api::user *user = m_data.getUser(userId);
        const td::td_api::chat *chat = m_data.getPrivateChatByUserId(userId);

        if (user && chat && isChatInContactList(*chat, user))
            updatePrivateChat(m_data, *chat, *user);
    }
}

void PurpleTdClient::updateGroup(td::td_api::object_ptr<td::td_api::basicGroup> group)
{
    if (!group) {
        purple_debug_warning(config::pluginId, "updateBasicGroup with null group\n");
        return;
    }
    purple_debug_misc(config::pluginId, "updateBasicGroup id=%d\n", group->id_);

    int32_t id       = group->id_;
    m_data.updateBasicGroup(std::move(group));

    // purple_blist_find_chat doesn't work if account is not connected
    if (purple_account_is_connected(m_account))
        updateBasicGroupChat(m_data, id);
}

void PurpleTdClient::updateSupergroup(td::td_api::object_ptr<td::td_api::supergroup> group)
{
    if (!group) {
        purple_debug_warning(config::pluginId, "updateSupergroup with null group\n");
        return;
    }
    purple_debug_misc(config::pluginId, "updateSupergroup id=%d\n", group->id_);

    int32_t id       = group->id_;
    m_data.updateSupergroup(std::move(group));

    // purple_blist_find_chat doesn't work if account is not connected
    if (purple_account_is_connected(m_account))
        updateSupergroupChat(m_data, id);
}

void PurpleTdClient::updateChat(const td::td_api::chat *chat)
{
    if (!chat) return;

    const td::td_api::user *privateChatUser = m_data.getUserByPrivateChat(*chat);
    int32_t                 basicGroupId    = getBasicGroupId(*chat);
    int32_t                 supergroupId    = getSupergroupId(*chat);
    purple_debug_misc(config::pluginId, "Update chat: %" G_GINT64_FORMAT " private user=%d basic group=%d supergroup=%d\n",
                      chat->id_, privateChatUser ? privateChatUser->id_ : 0, basicGroupId, supergroupId);

    // For chats, find_chat doesn't work if account is not yet connected, so just in case, don't
    // user find_buddy either
    if (!purple_account_is_connected(m_account))
        return;

    if (isChatInContactList(*chat, privateChatUser)) {
        if (privateChatUser)
            updatePrivateChat(m_data, *chat, *privateChatUser);

        // purple_blist_find_chat doesn't work if account is not connected
        if (basicGroupId) {
            requestBasicGroupMembers(basicGroupId);
            updateBasicGroupChat(m_data, basicGroupId);
        }
        if (supergroupId)
            updateSupergroupChat(m_data, supergroupId);
    } else {
        removeGroupChat(m_account, *chat);
    }
}

void PurpleTdClient::addChat(td::td_api::object_ptr<td::td_api::chat> chat)
{
    if (!chat) {
        purple_debug_warning(config::pluginId, "updateNewChat with null chat info\n");
        return;
    }

    purple_debug_misc(config::pluginId, "Add chat: '%s'\n", chat->title_.c_str());
    int64_t chatId = chat->id_;
    m_data.addChat(std::move(chat));
    updateChat(m_data.getChat(chatId));
}

void PurpleTdClient::handleUserChatAction(const td::td_api::updateUserChatAction &updateChatAction)
{
    const td::td_api::chat *chat = m_data.getChat(updateChatAction.chat_id_);
    if (!chat) {
        purple_debug_warning(config::pluginId, "Got user chat action for unknown chat %" G_GINT64_FORMAT "\n",
                             updateChatAction.chat_id_);
        return;
    }

    int32_t chatUserId = getUserIdByPrivateChat(*chat);
    if (chatUserId == 0) {
        purple_debug_misc(config::pluginId, "Ignoring user chat action for non-private chat %" G_GINT64_FORMAT "\n",
                          updateChatAction.chat_id_);
        return;
    }

    if (chatUserId != updateChatAction.user_id_)
        purple_debug_warning(config::pluginId, "Got user action for private chat %" G_GINT64_FORMAT " (with user %d) for another user %d\n",
                                updateChatAction.chat_id_, chatUserId, updateChatAction.user_id_);
    else if (updateChatAction.action_) {
        if (updateChatAction.action_->get_id() == td::td_api::chatActionCancel::ID) {
            purple_debug_misc(config::pluginId, "User (id %d) stopped chat action\n",
                                updateChatAction.user_id_);
            showUserChatAction(updateChatAction.user_id_, false);
        } else if (updateChatAction.action_->get_id() == td::td_api::chatActionStartPlayingGame::ID) {
            purple_debug_misc(config::pluginId, "User (id %d): treating chatActionStartPlayingGame as cancel\n",
                                updateChatAction.user_id_);
            showUserChatAction(updateChatAction.user_id_, false);
        } else {
            purple_debug_misc(config::pluginId, "User (id %d) started chat action (id %d)\n",
                                updateChatAction.user_id_, updateChatAction.action_->get_id());
            showUserChatAction(updateChatAction.user_id_, true);
        }
    }
}

void PurpleTdClient::showUserChatAction(int32_t userId, bool isTyping)
{
    const td::td_api::user *user = m_data.getUser(userId);
    if (user) {
        std::string userName = getPurpleBuddyName(*user);
        if (isTyping)
            serv_got_typing(purple_account_get_connection(m_account),
                            userName.c_str(), REMOTE_TYPING_NOTICE_TIMEOUT,
                            PURPLE_TYPING);
        else
            serv_got_typing_stopped(purple_account_get_connection(m_account),
                                    userName.c_str());
    }
}

static void showFailedContactMessage(void *handle, const std::string &errorMessage)
{
    std::string message = formatMessage(_("Failed to add contact: {}"), errorMessage);
    purple_notify_error(handle, _("Failed to add contact"), message.c_str(), NULL);
}

static int failedContactIdle(gpointer userdata)
{
    char *message = static_cast<char *>(userdata);
    showFailedContactMessage(NULL, message);
    free(message);
    return FALSE; // This idle callback will not be called again
}

static void notifyFailedContactDeferred(const std::string &message)
{
    g_idle_add(failedContactIdle, strdup(message.c_str()));
}

void PurpleTdClient::addContact(const std::string &purpleName, const std::string &alias,
                                const std::string &groupName)
{
    if (m_data.getUserByPhone(purpleName.c_str())) {
        purple_debug_info(config::pluginId, "User with phone number %s already exists\n", purpleName.c_str());
        return;
    }

    std::vector<const td::td_api::user *> users;
    m_data.getUsersByDisplayName(purpleName.c_str(), users);
    if (users.size() > 1) {
        notifyFailedContactDeferred("More than one user known with name '" + purpleName + "'");
        return;
    }

    if (users.size() == 1)
        addContactById(users[0]->id_, "", purpleName, groupName);
    else {
        td::td_api::object_ptr<td::td_api::contact> contact =
            td::td_api::make_object<td::td_api::contact>(purpleName, "", "", "", 0);
        td::td_api::object_ptr<td::td_api::importContacts> importReq =
            td::td_api::make_object<td::td_api::importContacts>();
        importReq->contacts_.push_back(std::move(contact));
        uint64_t requestId = m_transceiver.sendQuery(std::move(importReq),
                                                     &PurpleTdClient::importContactResponse);

        m_data.addPendingRequest<ContactRequest>(requestId, purpleName, alias, groupName, 0);
    }
}

void PurpleTdClient::addContactById(int32_t userId, const std::string &phoneNumber, const std::string &alias,
                                    const std::string &groupName)
{
    purple_debug_misc(config::pluginId, "Adding contact: id=%d alias=%s\n", userId, alias.c_str());
    std::string firstName, lastName;
    getNamesFromAlias(alias.c_str(), firstName, lastName);

    td::td_api::object_ptr<td::td_api::contact> contact =
        td::td_api::make_object<td::td_api::contact>(phoneNumber, firstName, lastName, "", userId);
    td::td_api::object_ptr<td::td_api::addContact> addContact =
        td::td_api::make_object<td::td_api::addContact>(std::move(contact), true);
    uint64_t newRequestId = m_transceiver.sendQuery(std::move(addContact),
                                                    &PurpleTdClient::addContactResponse);
    m_data.addPendingRequest<ContactRequest>(newRequestId, phoneNumber, alias, groupName, userId);
}

void PurpleTdClient::importContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    int32_t userId = 0;
    if (object->get_id() == td::td_api::importedContacts::ID) {
        td::td_api::object_ptr<td::td_api::importedContacts> reply =
            td::move_tl_object_as<td::td_api::importedContacts>(object);
        if (!reply->user_ids_.empty())
            userId = reply->user_ids_[0];
    }

    // For whatever reason, complaining at an earlier stage leads to error message not being shown in pidgin
    if (!isPhoneNumber(request->phoneNumber.c_str()))
        notifyFailedContact(formatMessage(_("{} is not a valid phone number"), request->phoneNumber));
    else if (userId)
        addContactById(userId, request->phoneNumber, request->alias, request->groupName);
    else
        notifyFailedContact(formatMessage(_("No user found with phone number '{}'"), request->phoneNumber));
}

void PurpleTdClient::addContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    if (object->get_id() == td::td_api::ok::ID) {
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(request->userId, false);
        uint64_t newRequestId = m_transceiver.sendQuery(std::move(createChat),
                                                        &PurpleTdClient::addContactCreatePrivateChatResponse);
        m_data.addPendingRequest(newRequestId, std::move(request));
    } else
        notifyFailedContact(getDisplayedError(object));
}

void PurpleTdClient::addContactCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    if (!object || (object->get_id() != td::td_api::chat::ID)) {
        purple_debug_misc(config::pluginId, "Failed to create private chat to %s\n",
                          request->phoneNumber.c_str());
        notifyFailedContact(getDisplayedError(object));
    }
}

void PurpleTdClient::notifyFailedContact(const std::string &errorMessage)
{
    showFailedContactMessage(purple_account_get_connection(m_account), errorMessage);
}

void PurpleTdClient::renameContact(const char *buddyName, const char *newAlias)
{
    int32_t userId = stringToUserId(buddyName);
    if (userId == 0) {
        purple_debug_warning(config::pluginId, "Cannot rename %s: not a valid id\n", buddyName);
        return;
    }

    std::string firstName, lastName;
    getNamesFromAlias(newAlias, firstName, lastName);
    auto contact    = td::td_api::make_object<td::td_api::contact>("", firstName, lastName, "", userId);
    auto addContact = td::td_api::make_object<td::td_api::addContact>(std::move(contact), true);
    m_transceiver.sendQuery(std::move(addContact), nullptr);
}

bool PurpleTdClient::joinChat(const char *chatName)
{
    int64_t                 id       = getTdlibChatId(chatName);
    const td::td_api::chat *chat     = m_data.getChat(id);
    int32_t                 purpleId = m_data.getPurpleChatId(id);
    PurpleConvChat         *conv     = NULL;

    if (!chat)
        purple_debug_warning(config::pluginId, "No telegram chat found for purple name %s\n", chatName);
    else if (!m_data.isGroupChatWithMembership(*chat))
        purple_debug_warning(config::pluginId, "Chat %s (%s) is not a group we a member of\n",
                             chatName, chat->title_.c_str());
    else if (purpleId) {
        conv = getChatConversation(m_data, *chat, purpleId);
        if (conv)
            purple_conversation_present(purple_conv_chat_get_conversation(conv));
    }

    return conv ? true : false;
}

int PurpleTdClient::sendGroupMessage(int purpleChatId, const char *message)
{
    const td::td_api::chat *chat = m_data.getChatByPurpleId(purpleChatId);

    if (!chat)
        purple_debug_warning(config::pluginId, "No chat found for purple id %d\n", purpleChatId);
    else if (!m_data.isGroupChatWithMembership(*chat))
        purple_debug_warning(config::pluginId, "purple id %d (chat %s) is not a group we a member of\n",
                             purpleChatId, chat->title_.c_str());
    else {
        transmitMessage(chat->id_, message, m_transceiver, m_data, &PurpleTdClient::sendMessageResponse);
        // Message shall not be echoed: tdlib will shortly present it as a new message and it will be displayed then
        return 0;
    }

    return -1;
}

bool PurpleTdClient::joinChatByLink(const char *inviteLink)
{
    auto     request   = td::td_api::make_object<td::td_api::joinChatByInviteLink>(inviteLink);
    uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::joinChatByLinkResponse);
    m_data.addPendingRequest<GroupJoinRequest>(requestId, inviteLink);

    return true;
}

void PurpleTdClient::joinChatByLinkResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupJoinRequest> request = m_data.getPendingRequest<GroupJoinRequest>(requestId);
    if (object && (object->get_id() == td::td_api::chat::ID)) {
        // If the chat was added with something like "Add chat" function from Pidgin, the chat in
        // contact list was created without id component (for if there was the id component,
        // tgprpl_chat_join would not have called PurpleTdClient::joinChatByLink).

        // So when updateNewChat came prior to this response (as it must have), a new chat with
        // correct id component (but without invite link component) was added to the contact list
        // by PurpleTdClient::addChat calling updateBasicGroupChat, or whatever happens for
        // supergroups.

        // Therefore, remove the original manually added chat, and keep the auto-added one.
        // Furthermore, user could have added same chat like that multiple times, in which case
        // remove all of them.
        if (request) {
            std::vector<PurpleChat *> obsoleteChats = findChatsByInviteLink(request->inviteLink);
            for (PurpleChat *chat: obsoleteChats)
                purple_blist_remove_chat(chat);
        }
    } else {
        std::string message = formatMessage(_("Failed to join chat: {}"), getDisplayedError(object));
        purple_notify_error(purple_account_get_connection(m_account), _("Failed to join chat"),
                            message.c_str(), NULL);
    }
}

void PurpleTdClient::sendMessageResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<SendMessageRequest> request = m_data.getPendingRequest<SendMessageRequest>(requestId);
    if (!request)
        return;
    if (object && (object->get_id() == td::td_api::message::ID)) {
        const td::td_api::message &message = static_cast<td::td_api::message &>(*object);
        m_data.addTempFileUpload(message.id_, request->tempFile);
    }
}

void PurpleTdClient::removeTempFile(int64_t messageId)
{
    std::string path = m_data.extractTempFileUpload(messageId);
    if (!path.empty()) {
        purple_debug_misc(config::pluginId, "Removing temporary file %s\n", path.c_str());
        remove(path.c_str());
    }
}

void PurpleTdClient::getUsers(const char *username, std::vector<const td::td_api::user *> &users)
{
    getUsersByPurpleName(username, users, m_data);
}

void PurpleTdClient::sendTyping(const char *buddyName, bool isTyping)
{
    int64_t chatId = getPrivateChatIdByPurpleName(buddyName, m_data, "send message");

    if (chatId != 0) {
        auto sendAction = td::td_api::make_object<td::td_api::sendChatAction>();
        sendAction->chat_id_ = chatId;
        if (isTyping)
            sendAction->action_ = td::td_api::make_object<td::td_api::chatActionTyping>();
        else
            sendAction->action_ = td::td_api::make_object<td::td_api::chatActionCancel>();
        m_transceiver.sendQuery(std::move(sendAction), nullptr);
    }
}

void PurpleTdClient::removeContactAndPrivateChat(const std::string &buddyName)
{
    int32_t userId = stringToUserId(buddyName.c_str());
    if (userId != 0) {
        const td::td_api::chat *chat   = m_data.getPrivateChatByUserId(userId);
        if (chat) {
            int64_t chatId = chat->id_;
            chat = nullptr;
            m_data.deleteChat(chatId); // Prevent re-creating buddy if any updateChat* or updateUser arrives

            auto deleteChat = td::td_api::make_object<td::td_api::deleteChatHistory>();
            deleteChat->chat_id_ = chatId;
            deleteChat->remove_from_chat_list_ = true;
            deleteChat->revoke_ = false;
            m_transceiver.sendQuery(std::move(deleteChat), nullptr);
        }

        auto removeContact = td::td_api::make_object<td::td_api::removeContacts>();
        removeContact->user_ids_.push_back(userId);
        m_transceiver.sendQuery(std::move(removeContact), nullptr);
    }
}
