#include "LetsPlayServer.h"

LetsPlayServer::LetsPlayServer(lib::filesystem::path& configFile) { config.LoadFrom(configFile); }

void LetsPlayServer::Run(std::uint16_t port) {
    if (port == 0) return;

    server.reset(new wcpp_server);

    try {
        server->set_access_channels(websocketpp::log::alevel::connect | websocketpp::log::alevel::disconnect);
        server->clear_access_channels(websocketpp::log::alevel::frame_payload | websocketpp::log::alevel::frame_header);

        server->init_asio();

        server->set_validate_handler(std::bind(&LetsPlayServer::OnValidate, this, ::_1));
        server->set_message_handler(std::bind(&LetsPlayServer::OnMessage, this, ::_1, ::_2));
        server->set_open_handler(std::bind(&LetsPlayServer::OnConnect, this, ::_1));
        server->set_close_handler(std::bind(&LetsPlayServer::OnDisconnect, this, ::_1));

        websocketpp::lib::error_code err;
        server->listen(port, err);

        if (err)
            throw std::runtime_error(std::string("Failed to listen on port ") +
                std::to_string(port));

        m_QueueThreadRunning = true;

        m_QueueThread = std::thread{[&]() { this->QueueThread(); }};

        // Skip having to connect, change username, addemu
        {
            std::unique_lock<std::mutex> lk(m_QueueMutex);
            m_WorkQueue.push(Command{kCommandType::AddEmu, {"emu1", "./core", "./rom"}, {}, ""});
            m_QueueNotifier.notify_one();
        }

        server->start_accept();
        server->run();

        this->Shutdown();
    } catch (websocketpp::exception const& e) {
        logger.err(e.what(), '\n');
    } catch (...) {
        throw;
    }
}

bool LetsPlayServer::OnValidate(websocketpp::connection_hdl hdl) {
    // TODO: Do bans here

    websocketpp::lib::error_code err;
    wcpp_server::connection_ptr cptr = server->get_con_from_hdl(hdl, err);

    boost::system::error_code ec;
    const auto& ep = cptr->get_raw_socket().remote_endpoint(ec);
    if (ec)
        return false;

    const boost::asio::ip::address& addr = ep.address();

    logger.log('[', addr.to_string(), "] <", hdl.lock(), "> validate");
    return true;
}

void LetsPlayServer::OnConnect(websocketpp::connection_hdl hdl) {
    {
        std::unique_lock lk(m_UsersMutex);
        std::shared_ptr<LetsPlayUser> user{new LetsPlayUser};
        user->setUsername("");

        websocketpp::lib::error_code err;
        wcpp_server::connection_ptr cptr = server->get_con_from_hdl(hdl, err);

        boost::system::error_code ec;
        const auto& ep = cptr->get_raw_socket().remote_endpoint(ec);
        if (!ec) {
            const boost::asio::ip::address& addr = ep.address();
            user->setIP(addr.to_string());
        }

        logger.log('[', user->IP(), "] <", hdl.lock(), "> connect");
        logger.log('<', hdl.lock(), "> -> ", user->uuid(), " -> [", user->IP(), ']');

        m_Users[hdl] = user;
    }
}

void LetsPlayServer::OnDisconnect(websocketpp::connection_hdl hdl) {
    LetsPlayUserHdl user_hdl;
    decltype(m_Users)::iterator search;
    {
        std::unique_lock lk(m_UsersMutex);
        search = m_Users.find(hdl);
        if (search == m_Users.end()) {
            logger.log("Couldn't find user who left in list\n");
            return;
        }
        user_hdl = search->second;
    }

    if (auto user = user_hdl.lock(); user && !user->connectedEmu().empty()) {
        {
            std::unique_lock lk(m_EmusMutex);
            // TODO: Check if emu exists
            m_Emus[user->connectedEmu()]->userDisconnected(user_hdl);
        }
        BroadcastToEmu(user->connectedEmu(),
                       LetsPlayProtocol::encode("leave", user->username()),
                       websocketpp::frame::opcode::text);

        logger.log(user->uuid(), " (", user->username(), ") left.");
    }

    {
        std::unique_lock lk(m_UsersMutex);

        // Double check is on purpose
        search = m_Users.find(hdl);
        if (search != m_Users.end()) m_Users.erase(search);
    }
}

void LetsPlayServer::OnMessage(websocketpp::connection_hdl hdl, wcpp_server::message_ptr msg) {
    const std::string& data = msg->get_payload();
    const auto decoded = LetsPlayProtocol::decode(data);

    if (decoded.empty()) return;
    // TODO: that switch case compile-time hash thing; its faster
    const auto& command = decoded.at(0);
    kCommandType t = kCommandType::Unknown;

    if (command == "list")  // No params
        t = kCommandType::List;
    else if (command == "chat")  // message
        t = kCommandType::Chat;
    else if (command == "username")  // newname
        t = kCommandType::Username;
    else if (command == "button")  // button id, 0/1 for keyup/keydown
        t = kCommandType::Button;
    else if (command == "connect")  // emuid
        t = kCommandType::Connect;
    else if (command == "turn")  // No params
        t = kCommandType::Turn;
    else if (command == "add")
        t = kCommandType::AddEmu;
    else if (command == "admin")
        t = kCommandType::Admin;
    else if (command == "shutdown")
        t = kCommandType::Shutdown;
    else
        return;

    EmuID_t emuID;
    LetsPlayUserHdl user_hdl;
    if (std::unique_lock lk(m_UsersMutex); m_Users.find(hdl) != m_Users.end()) {
        user_hdl = m_Users[hdl];
        if (auto user = user_hdl.lock())
            emuID = user->connectedEmu();
    }

    if (auto user = user_hdl.lock())
        logger.log(user->uuid(), " (", user->username(), ") raw: '", data, '\'');

    if (auto user = user_hdl.lock()) {
        if (t == kCommandType::Shutdown) {
            if (!user->hasAdmin)
                return;
            else
                this->Shutdown();
        }
    }

    Command c{t, std::vector<std::string>(), hdl, emuID, user_hdl};
    if (decoded.size() > 1)
        c.params = std::vector<std::string>(decoded.begin() + 1, decoded.end());

    {
        std::unique_lock lk(m_QueueMutex);
        m_WorkQueue.push(c);
    }

    m_QueueNotifier.notify_one();
}

void LetsPlayServer::Shutdown() {
    // Run this function once
    static bool shuttingdown = false;

    if (!shuttingdown)
        shuttingdown = true;
    else
        return;

    // Stop the work thread loop
    m_QueueThreadRunning = false;
    logger.log("Stopping work thread...");
    {
        logger.log("Emptying the queue...");
        // Empty the queue ...
        std::unique_lock lk((m_QueueMutex));
        while (!m_WorkQueue.empty()) m_WorkQueue.pop();
        // ... Except for a shutdown command
        m_WorkQueue.push(Command{kCommandType::Shutdown, std::vector<std::string>(),
                                 websocketpp::connection_hdl(), ""});
    }

    logger.log("Stopping listen...");
    // Stop listening so the queue doesn't grow any more
    websocketpp::lib::error_code err;
    server->stop_listening(err);
    if (err)
        logger.err("Error stopping listen ", err.message());
    // Wake up the turn and work threads
    logger.log("Waking up work thread...");
    m_QueueNotifier.notify_one();
    // Wait until they stop looping
    logger.log("Waiting for work thread to stop...");
    m_QueueThread.join();

    // Close every connection
    {
        logger.log("Closing every connection...");
        std::unique_lock lk(m_UsersMutex);
        for ([[maybe_unused]] const auto&[hdl, _] : m_Users)
            if (!hdl.expired())
                server->close(hdl, websocketpp::close::status::normal, "Closing", err);
    }
}

void LetsPlayServer::QueueThread() {
    while (m_QueueThreadRunning) {
        {
            std::unique_lock lk(m_QueueMutex);
            // Use std::condition_variable::wait Predicate?
            while (m_WorkQueue.empty()) m_QueueNotifier.wait(lk);

            if (!m_WorkQueue.empty()) {
                auto& command = m_WorkQueue.front();

                switch (command.type) {
                    case kCommandType::Chat: {
                        // Chat has only one, the message
                        if (command.params.size() != 1) break;

                        if (auto user = command.user_hdl.lock()) {
                            if (user->username().empty())
                                break;

                            // Message only has values in the range of typeable
                            // ascii characters excluding \n and \t
                            if (!LetsPlayServer::isAsciiStr(command.params[0])) break;

                            std::uint64_t maxMessageSize;
                            {
                                std::shared_lock lkk(config.mutex);
                                nlohmann::json& data = config.config["serverConfig"]["maxMessageSize"];

                                // TODO: Warning on invalid data type (logging system implemented)
                                if (!data.is_number_unsigned()) {
                                    maxMessageSize =
                                        LetsPlayConfig::defaultConfig["serverConfig"]["maxMessageSize"];
                                } else {
                                    maxMessageSize = data;
                                }
                            }

                            if (LetsPlayServer::escapedSize(command.params[0]) > maxMessageSize) break;

                            if (auto user = command.user_hdl.lock(); user) {
                                BroadcastAll(
                                    LetsPlayProtocol::encode("chat", user->username(), command.params[0]),
                                    websocketpp::frame::opcode::text
                                );
                                logger.log(user->uuid(), " (", user->username(), "): '", command.params[0], '\'');
                            }
                        }
                    }
                        break;
                    case kCommandType::Username: {
                        // Username has only one param, the username
                        if (command.params.size() != 1) break;

                        if (auto user = command.user_hdl.lock()) {
                            const auto& newUsername = command.params.at(0);
                            const auto oldUsername = user->username();

                            const bool justJoined = oldUsername.empty();

                            // Ignore no change if haven't just joined
                            if (newUsername == oldUsername && !justJoined) {
                                // Treat as invalid if they haven't just joined and they tried to request a new username
                                // that's the same as their current one
                                BroadcastOne(
                                    LetsPlayProtocol::encode("username", oldUsername, oldUsername),
                                    command.hdl);
                                logger.log(user->uuid(),
                                           " (",
                                           user->username(),
                                           ") failed username change to : '",
                                           newUsername,
                                           '\'');
                            }

                            std::uint64_t maxUsernameLen, minUsernameLen;
                            {
                                std::shared_lock lkk(config.mutex);

                                nlohmann::json& max = config.config["serverConfig"]["maxUsernameLength"],
                                    min = config.config["serverConfig"]["minUsernameLength"];

                                // TODO: Warning on invalid data type (logging system implemented)
                                if (!max.is_number_unsigned())
                                    maxUsernameLen = LetsPlayConfig::defaultConfig["serverConfig"]["maxUsernameLength"];
                                else
                                    maxUsernameLen = max;

                                if (!min.is_number_unsigned())
                                    minUsernameLen = LetsPlayConfig::defaultConfig["serverConfig"]["minUsernameLength"];
                                else
                                    minUsernameLen = min;
                            }

                            // Size based checks
                            if (newUsername.size() > maxUsernameLen
                                || newUsername.size() < minUsernameLen) {
                                if (justJoined)
                                    GiveGuest(command.hdl, command.user_hdl);
                                else {
                                    BroadcastOne(
                                        LetsPlayProtocol::encode("username", oldUsername, oldUsername),
                                        command.hdl);
                                    logger.log(user->uuid(),
                                               " (",
                                               user->username(),
                                               ") failed username change to '",
                                               newUsername,
                                               "' due to length.");
                                }
                                break;
                            }

                            // Content based checks
                            if (newUsername.front() == ' ' || newUsername.back() == ' ' // Spaces at beginning/end
                                || !LetsPlayServer::isAsciiStr(newUsername)         // Non-ascii printable characters
                                || (newUsername.find("  ") != std::string::npos)) { // Double spaces inside username
                                if (justJoined)
                                    GiveGuest(command.hdl, command.user_hdl);
                                else {
                                    BroadcastOne(
                                        LetsPlayProtocol::encode("username", oldUsername, oldUsername),
                                        command.hdl);
                                    logger.log(user->uuid(),
                                               " (",
                                               user->username(),
                                               ") failed username change to '",
                                               newUsername,
                                               "' due to content.");
                                }
                                break;
                            }

                            // Finally, check if username is already taken
                            if (UsernameTaken(newUsername, user->uuid())) {
                                if (justJoined)
                                    GiveGuest(command.hdl, command.user_hdl);
                                else {
                                    BroadcastOne(
                                        LetsPlayProtocol::encode("username", oldUsername, oldUsername),
                                        command.hdl);
                                    logger.log(user->uuid(),
                                               " (",
                                               user->username(),
                                               ") failed username change to '",
                                               newUsername,
                                               "' because its already taken.");
                                }
                                break;
                            }

                            /*
                             * If all checks were passed, set username and broadcast to the person that they have a new
                             * username, and send a join/rename to everyone if the person just joined/has been around
                             */
                            user->setUsername(newUsername);

                            BroadcastOne(
                                LetsPlayProtocol::encode("username", oldUsername, newUsername),
                                command.hdl
                            );

                            logger.log(user->uuid(), " (", user->username(), ") set username to '", newUsername, '\'');

                            if (justJoined) { // Send a join message
                                BroadcastToEmu(
                                    user->connectedEmu(),
                                    LetsPlayProtocol::encode("join", user->username()),
                                    websocketpp::frame::opcode::text);

                                logger.log(user->uuid(), " (", user->username(), ") joined.");
                            } else { // Tell everyone on the emu someone changed their username
                                BroadcastToEmu(user->connectedEmu(),
                                               LetsPlayProtocol::encode("rename", oldUsername, newUsername),
                                               websocketpp::frame::opcode::text);
                                logger.log(user->uuid(),
                                           " (",
                                           user->username(),
                                           "): ",
                                           oldUsername,
                                           " is now known as ",
                                           newUsername);
                            }
                        }
                    }
                        break;
                    case kCommandType::List: {
                        if (!command.params.empty()) break;

                        if (auto user = command.user_hdl.lock()) {
                            logger.log(user->uuid(), " (", user->username(), ") requested a user list.");
                        }

                        std::vector<std::string> message;
                        message.emplace_back("list");

                        {
                            std::unique_lock lkk(m_UsersMutex);
                            for (auto&[hdl, user] : m_Users)
                                if (auto commandUser = command.user_hdl.lock(); commandUser) {
                                    if ((commandUser->connectedEmu() == user->connectedEmu()) &&
                                        !hdl.expired())
                                        message.push_back(user->username());
                                }
                        }

                        BroadcastOne(LetsPlayProtocol::encode(message), command.hdl);
                    }
                        break;
                    case kCommandType::Turn: {
                        if (!command.params.empty()) break;

                        if (auto user = command.user_hdl.lock()) {
                            logger.log(user->uuid(),
                                       " (",
                                       user->username(),
                                       ") requested a turn."
                                       "user->requestedTurn: ",
                                       (user->requestedTurn) == true,
                                       " user->connectedEmu: ",
                                       user->connectedEmu());

                            if (user->connectedEmu().empty() || user->requestedTurn)
                                break;

                            std::unique_lock lkk(m_EmusMutex);
                            if (auto emu = m_Emus[command.emuID]; emu) {
                                user->requestedTurn = true;
                                emu->addTurnRequest(command.user_hdl);
                            }
                        }
                    }
                        break;
                    case kCommandType::Shutdown:break;
                    case kCommandType::Connect: {
                        if (auto user = command.user_hdl.lock()) {
                            if (command.params.size() != 1 || user->username().empty()) {
                                LetsPlayServer::BroadcastOne(LetsPlayProtocol::encode("connect", false), command.hdl);
                                logger.log(user->uuid(),
                                           " (",
                                           user->username(),
                                           ") failed to connect to an emulator (1st check).");
                                break;
                            }

                            // Check if the emu that the connect thing that was sent exists
                            if (std::unique_lock lkk(m_EmusMutex); m_Emus.find(command.params[0]) == m_Emus.end()) {
                                LetsPlayServer::BroadcastOne(LetsPlayProtocol::encode("connect", false),
                                                             command.hdl);
                                logger.log(user->uuid(),
                                           " (",
                                           user->username(),
                                           ") tried to connect to an emulator that doesn't exst.");
                                break;
                            }

                            // NOTE: Can remove check and allow on the fly
                            // switching once the transition between being
                            // connected to A and being connected to B is
                            // figured out
                            if (!(user->connectedEmu().empty())) break;

                            BroadcastToEmu(command.params[0],
                                           LetsPlayProtocol::encode("join", user->username()),
                                           websocketpp::frame::opcode::text);

                            user->setConnectedEmu(command.params[0]);
                            {
                                std::unique_lock lkk(m_EmusMutex);
                                m_Emus[user->connectedEmu()]->userConnected(command.user_hdl);
                            }

                            BroadcastOne(LetsPlayProtocol::encode("connect", true), command.hdl);

                            logger.log(user->uuid(), " (", user->username(), ") connected to ", command.params[0]);

                            std::uint64_t maxUsernameLen, minUsernameLen, maxMessageSize;
                            {
                                std::shared_lock lkk(config.mutex);

                                nlohmann::json& max = config.config["serverConfig"]["maxUsernameLength"],
                                    min = config.config["serverConfig"]["minUsernameLength"],
                                    msgMax = config.config["serverConfig"]["maxMessageSize"];

                                // TODO: Warning on invalid data type (logging system implemented)
                                if (!max.is_number_unsigned())
                                    maxUsernameLen = LetsPlayConfig::defaultConfig["serverConfig"]["maxUsernameLength"];
                                else
                                    maxUsernameLen = max;

                                if (!min.is_number_unsigned())
                                    minUsernameLen = LetsPlayConfig::defaultConfig["serverConfig"]["minUsernameLength"];
                                else
                                    minUsernameLen = min;

                                if (!msgMax.is_number_unsigned())
                                    maxMessageSize = LetsPlayConfig::defaultConfig["serverConfig"]["maxMessageSize"];
                                else
                                    maxMessageSize = msgMax;
                            }

                            BroadcastOne(
                                LetsPlayProtocol::encode("emuinfo",
                                                         minUsernameLen,
                                                         maxUsernameLen,
                                                         maxMessageSize,
                                                         user->connectedEmu()),
                                command.hdl
                            );
                        }
                    }
                        break;
                    case kCommandType::Button: {  // button/leftStick/rightStick, button id, value as int16
                        if (command.params.size() != 3) break;

                        if (auto user = command.user_hdl.lock(); user && !user->hasTurn) break;

                        const auto &buttonType = command.params[0];
                        std::int16_t id, value;

                        // Spaghet
                        {
                            std::stringstream ss{command.params[1]};
                            ss >> id;
                            if (!ss)
                                break;
                        }
                        {
                            std::stringstream ss{command.params[2]};
                            ss >> value;
                            if (!ss)
                                break;
                        }

                        if (auto user = command.user_hdl.lock())
                            logger.log(user->uuid(),
                                       " (",
                                       user->username(),
                                       ") sent a '",
                                       buttonType,
                                       "' update with id '",
                                       id,
                                       "' and value '",
                                       value,
                                       '\'');

                        if (id < 0)
                            break;

                        if (!command.emuID.empty()) {
                            std::unique_lock lkk(m_EmusMutex);
                            if (buttonType == "button") {
                                if (id > 15)
                                    break;
                                m_Emus[command.emuID]->joypad->updateValue(RETRO_DEVICE_INDEX_ANALOG_BUTTON, id, value);
                            } else if (buttonType == "leftStick") {
                                if (id > 1)
                                    break;
                                m_Emus[command.emuID]->joypad->updateValue(RETRO_DEVICE_INDEX_ANALOG_LEFT, id, value);
                            } else if (buttonType == "rightStick") {
                                if (id > 1)
                                    break;
                                m_Emus[command.emuID]->joypad->updateValue(RETRO_DEVICE_INDEX_ANALOG_RIGHT, id, value);
                            }
                        }
                    }
                        break;
                    case kCommandType::AddEmu: {  // emu, libretro core
                        // path, rom path
                        // TODO:: Add file path checks
                        if (command.params.size() != 3) break;

                        if (auto user = command.user_hdl.lock()) {
                            if (!user->hasAdmin)
                                break;
                        }

                        auto& id = command.params[0];
                        const auto& corePath = command.params[1];
                        const auto& romPath = command.params[2];

                        {
                            std::unique_lock lkk(m_EmuThreadMutex);
                            m_EmulatorThreads.emplace_back(
                                std::thread(EmulatorController::Run, corePath, romPath, this, id));
                        }
                    }
                        break;
                    case kCommandType::Admin: {
                        if (command.params.size() != 1) break;

                        if (auto user = command.user_hdl.lock()) {
                            if (user->adminAttempts >= 3)
                                break;
                        }

                        std::string salt, expectedHash;
                        {
                            std::shared_lock lkk(config.mutex);

                            nlohmann::json& jsalt = config.config["serverConfig"]["salt"],
                                jhash = config.config["serverConfig"]["adminHash"];

                            if (!jsalt.is_string()) {
                                jsalt = LetsPlayConfig::defaultConfig["serverConfig"]["salt"];
                            }

                            if (!jhash.is_string()) {
                                jhash = LetsPlayConfig::defaultConfig["serverConfig"]["adminHash"];
                            }

                            salt = jsalt;
                            expectedHash = jhash;
                        }
                        std::string hashed = md5(command.params[0] + salt);

                        logger.log(salt, ' ', expectedHash, ' ', hashed);

                        if (auto user = command.user_hdl.lock()) {
                            if (hashed == expectedHash) {
                                user->hasAdmin = true;
                            } else {
                                user->adminAttempts++;
                            }

                            BroadcastOne(
                                LetsPlayProtocol::encode("admin", (user->hasAdmin) == true),
                                command.hdl
                            );
                        }
                    }
                        break;
                    case kCommandType::Pong:
                        if (auto user = command.user_hdl.lock())
                            user->updateLastPong();
                        break;
                    case kCommandType::RemoveEmu:
                    case kCommandType::StopEmu:
                    case kCommandType::Config:
                    case kCommandType::Unknown:
                        // Unimplemented
                        break;
                    default:break;
                }
                m_WorkQueue.pop();
            }
        }
    }
}

void LetsPlayServer::PingThread() {
    const auto ping = LetsPlayProtocol::encode("ping");
    while(true) {
        for (auto&[hdl, user] : m_Users) {
            websocketpp::lib::error_code ec;

            // Check if should d/c
            if (user->shouldDisconnect()) {
                server->close(hdl, websocketpp::close::status::normal, "Timed out.", ec);
                continue;
            }

            // Send a ping if not
            if(!hdl.expired())
                server->send(hdl, ping, websocketpp::frame::opcode::text);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void LetsPlayServer::BroadcastAll(const std::string& data, websocketpp::frame::opcode::value op) {
    std::unique_lock lk(m_UsersMutex, std::try_to_lock);
    for (auto&[hdl, user] : m_Users) {
        if (websocketpp::lib::error_code ec; !user->username().empty() && user->connected && !hdl.expired())
            server->send(hdl, data, op, ec);
    }
}

void LetsPlayServer::BroadcastOne(const std::string&& data, websocketpp::connection_hdl hdl) {
    websocketpp::lib::error_code ec;
    server->send(hdl, data, websocketpp::frame::opcode::text, ec);
}

void LetsPlayServer::BroadcastToEmu(const EmuID_t& id, const std::string& message,
                                    websocketpp::frame::opcode::value op) {
    std::unique_lock lk(m_UsersMutex, std::try_to_lock);
    for (auto&[hdl, user] : m_Users) {
        if (websocketpp::lib::error_code ec; user->connectedEmu() == id && !user->username().empty() && user->connected
            && !hdl.expired())
            server->send(hdl, message, op, ec);
    }
}

void LetsPlayServer::GiveGuest(websocketpp::connection_hdl hdl, LetsPlayUserHdl user_hdl) {
    // TODO: Custom guest usernames? (i.e. being able to specify player##### in config)
    if (auto user = user_hdl.lock()) {
        std::string validUsername;
        do {
            validUsername = "guest";
            validUsername += std::to_string(rnd::nextInt() % 100000);
        } while (UsernameTaken(validUsername, user->uuid()));

        const std::string oldUsername = user->username();
        user->setUsername(validUsername);
        // Send valid username
        BroadcastOne(
            LetsPlayProtocol::encode("username", oldUsername, validUsername),
            hdl
        );
        logger.log(user->uuid(), " (", oldUsername, ") given new username '", user->username(), '\'');
    }
}

bool LetsPlayServer::UsernameTaken(const std::string& username, const std::string& uuid) {
    std::unique_lock lkk(m_UsersMutex);
    for (auto&[hdl, user] : m_Users) {
        if (user->uuid() != uuid &&
            user->username() == username && user->connected && !hdl.expired()) {
            return true;
        }
    }
    return false;
}

void LetsPlayServer::AddEmu(const EmuID_t& id, EmulatorControllerProxy *emu) {
    std::unique_lock lk(m_EmusMutex);
    m_Emus[id] = emu;
}


bool LetsPlayServer::isAsciiStr(const std::string& str) {
    return std::all_of(str.begin(), str.end(),
                       [](const char c) { return (c >= ' ') && (c <= '~'); });
}

size_t LetsPlayServer::escapedSize(const std::string& str) {
    // matches \uXXXX, \xXX, and \u{1XXXX}
    static const std::regex re{
        R"((\\x[\da-f]{2}|\\u[\da-f]{4}|\\u\{1[\da-f]{4}\}))"};
    const std::string output = std::regex_replace(str, re, "X");
    return output.size();
}

void LetsPlayServer::SendFrame(const EmuID_t& id) {
    thread_local static tjhandle _jpegCompressor = tjInitCompress();
    thread_local static long unsigned int _jpegBufferSize = 0;
    thread_local static std::uint8_t *jpegData{nullptr};
    Frame frame = [&]() {
        std::unique_lock lk(m_EmusMutex);
        auto emu = m_Emus[id];
        return emu->getFrame();
    }();

    // currentBuffer was nullptr
    if (frame.width == 0 || frame.height == 0) return;

    // TODO: Make this function faster by making this update less often
    unsigned quality = LetsPlayConfig::defaultConfig["serverConfig"]["jpegQuality"];
    if (std::shared_lock lk(config.mutex); config.config["serverConfig"].count("jpegQuality")) {
        nlohmann::json& value = config.config["serverConfig"]["jpegQuality"];
        if (value.is_number() && (value <= 100) && (value >= 1)) quality = value;
    }
    long unsigned int jpegSize = _jpegBufferSize;
    tjCompress2(_jpegCompressor, frame.data.data(), frame.width, frame.width * 3, frame.height,
                TJPF_RGB, &jpegData, &jpegSize, TJSAMP_420, quality, TJFLAG_ACCURATEDCT);

    _jpegBufferSize = _jpegBufferSize >= jpegSize ? _jpegBufferSize : jpegSize;

    std::unique_lock lk(m_UsersMutex);
    for (auto&[hdl, user] : m_Users) {
        if (user->connectedEmu() == id && user->connected && !hdl.expired()) {
            websocketpp::lib::error_code ec;
            server->send(hdl, jpegData, jpegSize, websocketpp::frame::opcode::binary, ec);
        }
    }
}

std::string LetsPlayServer::escapeTilde(std::string str) {
    if (str.front() == '~') {
        const char *homePath = std::getenv("HOME");
        if (!homePath) {
            std::cerr << "Tilde path was specified but couldn't retrieve "
                         "actual home path. Check if $HOME was declared.\n";
            return ".";
        }

        str.erase(0, 1);
        str.insert(0, homePath);
    }
    return str;
}

