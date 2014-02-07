#pragma once
/**
 * @file
 * @brief Protocol set.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <map>
#include <string>
#include <memory>
#include "cybozu/socket.hpp"
#include "packet.hpp"
#include "util.hpp"
#include "walb_logger.hpp"
#include "server_util.hpp"

namespace walb {
namespace protocol {

/**
 * RETURN:
 *   Server ID.
 */
static inline std::string run1stNegotiateAsClient(
    cybozu::Socket &sock,
    const std::string &clientId, const std::string &protocolName)
{
    packet::Packet packet(sock);
    packet.write(clientId);
    packet.write(protocolName);
    packet::Version ver(sock);
    ver.send();
    std::string serverId;
    packet.read(serverId);

    ProtocolLogger logger(clientId, serverId);
    packet::Answer ans(sock);
    int err;
    std::string msg;
    if (!ans.recv(&err, &msg)) {
        std::string s = cybozu::util::formatString(
            "received NG: err %d msg %s", err, msg.c_str());
        logger.error(s);
        throw std::runtime_error(s);
    }
    return serverId;
}

/**
 * Parameters for commands as a client.
 */
struct ClientParams
{
    cybozu::Socket &sock;
    ProtocolLogger &logger;
    const std::atomic<bool> &forceQuit;
    const std::vector<std::string> &params;

    ClientParams(
        cybozu::Socket &sock0,
        ProtocolLogger &logger0,
        const std::atomic<bool> &forceQuit0,
        const std::vector<std::string> &params0)
        : sock(sock0)
        , logger(logger0)
        , forceQuit(forceQuit0)
        , params(params0) {
    }
};

/**
 * Client handler type.
 */
using ClientHandler = void (*)(ClientParams &);

static inline void clientDispatch(
    const std::string& protocolName, cybozu::Socket& sock, ProtocolLogger& logger,
    const std::atomic<bool> &forceQuit, const std::vector<std::string> &params,
    const std::map<std::string, ClientHandler> &handlers)
{
    auto it = handlers.find(protocolName);
    if (it != handlers.cend()) {
        ClientHandler h = it->second;
        ClientParams p(sock, logger, forceQuit, params);
        h(p);
    } else {
        throw cybozu::Exception("dispatch:receive OK but protocol not found.") << protocolName;
    }
}

/**
 * @clientId will be set.
 * @protocol will be set.
 *
 * This function will process shutdown protocols.
 * For other protocols, this function will do only the common negotiation.
 *
 * RETURN:
 *   true if the protocol has finished or failed that is there is nothing to do.
 *   otherwise false.
 */
static inline bool run1stNegotiateAsServer(
    cybozu::Socket &sock, const std::string &serverId,
    std::string &protocolName,
    std::string &clientId,
    std::atomic<walb::server::ProcessStatus> &procStat)
{
    packet::Packet packet(sock);

    LOGi_("run1stNegotiateAsServer start\n");
    packet.read(clientId);
    LOGi_("clientId: %s\n", clientId.c_str());
    packet.read(protocolName);
    LOGi_("protocolName: %s\n", protocolName.c_str());
    packet::Version ver(sock);
    bool isVersionSame = ver.recv();
    LOGi_("isVersionSame: %d\n", isVersionSame);
    packet.write(serverId);

    ProtocolLogger logger(serverId, clientId);
    packet::Answer ans(sock);

    /* Server shutdown commands. */
    if (protocolName == "graceful-shutdown") {
        procStat = walb::server::ProcessStatus::GRACEFUL_SHUTDOWN;
        logger.info("graceful shutdown.");
        ans.ok();
        return true;
    } else if (protocolName == "force-shutdown") {
        procStat = walb::server::ProcessStatus::FORCE_SHUTDOWN;
        logger.info("force shutdown.");
        ans.ok();
        return true;
    }

    if (!isVersionSame) {
        std::string msg = cybozu::util::formatString(
            "Version differ: client %" PRIu32 " server %" PRIu32 ""
            , ver.get(), packet::VERSION);
        logger.warn(msg);
        ans.ng(1, msg);
        return true;
    }
    ans.ok();
    logger.info("initial negotiation succeeded: %s", protocolName.c_str());
    return false;

    /* Here command existance has not been checked yet. */
}

/**
 * Parameters for commands as a server.
 */
struct ServerParams
{
    cybozu::Socket &sock;
    ProtocolLogger &logger;
    const std::string &baseDirStr;
    const std::atomic<bool> &forceQuit;
    std::atomic<walb::server::ProcessStatus> &procStat;

    ServerParams(
        cybozu::Socket &sock0,
        ProtocolLogger &logger0,
        const std::string &baseDirStr0,
        const std::atomic<bool> &forceQuit0,
        std::atomic<walb::server::ProcessStatus> &procStat0)
        : sock(sock0)
        , logger(logger0)
        , baseDirStr(baseDirStr0)
        , forceQuit(forceQuit0)
        , procStat(procStat0) {
    }
};

/**
 * Server handler type.
 */
using ServerHandler = void (*)(ServerParams &);

/**
 * Server dispatcher.
 */
static inline void serverDispatch(
    cybozu::Socket &sock, const std::string &serverId, const std::string &baseDirStr,
    const std::atomic<bool> &forceQuit,
    std::atomic<walb::server::ProcessStatus> &procStat,
    const std::map<std::string, ServerHandler> &handlers) noexcept
{
    std::string clientId, protocolName;
    if (run1stNegotiateAsServer(sock, serverId, protocolName, clientId, procStat)) {
        /* The protocol has finished or failed. */
        return;
    }
    ProtocolLogger logger(serverId, clientId);
    try {
        auto it = handlers.find(protocolName);
        if (it != handlers.cend()) {
            ServerHandler h = it->second;
            ServerParams p(sock, logger, baseDirStr, forceQuit, procStat);
            h(p);
        } else {
            throw cybozu::Exception("bad protocolName") << protocolName;
        }
    } catch (std::exception &e) {
        logger.error("runlAsServer failed: %s", e.what());
    } catch (...) {
        logger.error("runAsServer failed: unknown error.");
    }
}

static inline void sendStrVec(
    cybozu::Socket &sock,
    const std::vector<std::string> &v, size_t numToSend, const char *msg, bool doAck = true)
{
    if (v.size() != numToSend) {
        throw cybozu::Exception(msg) << "bad size" << numToSend << v.size();
    }
    packet::Packet packet(sock);
    for (size_t i = 0; i < numToSend; i++) {
        if (v[i].empty()) {
            throw cybozu::Exception(msg) << "empty string" << i;
        }
    }
    packet.write(v);

	if (doAck) {
	    packet::Ack(sock).recv();
	}
}

static inline std::vector<std::string> recvStrVec(
    cybozu::Socket &sock, size_t numToRecv, const char *msg, bool doAck = true)
{
    packet::Packet packet(sock);
    std::vector<std::string> v;
    packet.read(v);
    if (v.size() != numToRecv) {
        throw cybozu::Exception(msg) << "bad size" << numToRecv << v.size();
    }
    for (size_t i = 0; i < numToRecv; i++) {
        if (v[i].empty()) {
            throw cybozu::Exception(msg) << "empty string" << i;
        }
    }
    if (doAck) {
        packet::Ack(sock).send();
    }
    return v;
}

}} // namespace walb::protocol


