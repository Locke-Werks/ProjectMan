#include "server.h"

#include "projects.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace pm::mcp {
namespace {

// JSON-RPC 2.0 error codes. The negative range below -32000 is reserved by the
// spec and these are its assigned members.
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;

// Protocol revisions this server behaves identically under. A tools-only server
// has nothing that differs between them, so the client's choice is honoured
// when it names one of these and the newest is offered otherwise.
constexpr const char* kSupportedProtocols[] = {
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};
constexpr const char* kDefaultProtocol = kSupportedProtocols[0];

json::Value errorObject(int code, const std::string& message)
{
    return json::makeObject({
        { "code", json::makeInt(code) },
        { "message", json::makeString(message) },
    });
}

json::Value makeResponse(const json::Value& id, json::Value result)
{
    return json::makeObject({
        { "jsonrpc", json::makeString("2.0") },
        { "id", id },
        { "result", std::move(result) },
    });
}

json::Value makeError(const json::Value& id, int code, const std::string& message)
{
    return json::makeObject({
        { "jsonrpc", json::makeString("2.0") },
        { "id", id },
        { "error", errorObject(code, message) },
    });
}

json::Value textResult(const std::string& text, bool isError)
{
    return json::makeObject({
        { "content", json::makeArray({ json::makeObject({
                         { "type", json::makeString("text") },
                         { "text", json::makeString(text) },
                     }) }) },
        { "isError", json::makeBool(isError) },
    });
}

json::Value toolDescriptor()
{
    // An empty properties object with additionalProperties false: the tool
    // takes nothing, and saying so explicitly stops a client inventing
    // arguments for it.
    json::Value schema = json::makeObject({
        { "type", json::makeString("object") },
        { "properties", json::makeObject({}) },
        { "additionalProperties", json::makeBool(false) },
    });

    return json::makeObject({
        { "name", json::makeString(kToolName) },
        { "title", json::makeString("Get projects") },
        { "description",
          json::makeString(
              "Report every project under the ProjectMan projects root: its path, "
              "kind, git state (branch, upstream, ahead/behind, staged, unstaged, "
              "untracked, conflicted, stashes, last commit) and its outstanding "
              "work (open pull requests, open issues, unchecked checklist items, "
              "and Claude Code session state). Read-only: it reports and changes "
              "nothing.") },
        { "inputSchema", std::move(schema) },
    });
}

std::string negotiateProtocol(const json::Value& params)
{
    const json::Value* requested = params.find("protocolVersion");
    if (!requested || requested->type != json::Value::Type::String)
        return kDefaultProtocol;

    for (const char* known : kSupportedProtocols) {
        if (requested->string == known)
            return requested->string;
    }
    return kDefaultProtocol;
}

} // namespace

bool handleMessage(const json::Value& request, json::Value* response)
{
    if (request.type != json::Value::Type::Object) {
        *response = makeError(json::makeNull(), kInvalidRequest,
                              "a JSON-RPC message must be an object");
        return true;
    }

    const json::Value* idField     = request.find("id");
    const json::Value* methodField = request.find("method");

    // No id means a notification. Nothing may be sent back, even for an error:
    // a response carrying no id is uncorrelatable, and one carrying a made-up
    // id is worse.
    const bool  wantsReply = idField != nullptr && !idField->isNull();
    json::Value id         = wantsReply ? *idField : json::makeNull();

    if (!methodField || methodField->type != json::Value::Type::String) {
        if (!wantsReply)
            return false;
        *response = makeError(id, kInvalidRequest, "missing method");
        return true;
    }

    const std::string& method = methodField->string;

    if (method == "initialize") {
        if (!wantsReply)
            return false;

        json::Value params = request.find("params") ? *request.find("params")
                                                    : json::makeObject({});

        *response = makeResponse(
            id, json::makeObject({
                    { "protocolVersion", json::makeString(negotiateProtocol(params)) },
                    { "capabilities", json::makeObject({
                                          { "tools", json::makeObject({}) },
                                      }) },
                    { "serverInfo", json::makeObject({
                                        { "name", json::makeString(kServerName) },
                                        { "title", json::makeString("ProjectMan") },
                                        { "version", json::makeString(PM_VERSION_STRING) },
                                    }) },
                }));
        return true;
    }

    // Notifications the client sends that this server has nothing to do about.
    // Answering any of them would be a protocol violation.
    if (method == "notifications/initialized" || method == "notifications/cancelled"
        || method.rfind("notifications/", 0) == 0) {
        return false;
    }

    if (method == "ping") {
        if (!wantsReply)
            return false;
        *response = makeResponse(id, json::makeObject({}));
        return true;
    }

    if (method == "tools/list") {
        if (!wantsReply)
            return false;
        *response = makeResponse(
            id, json::makeObject({
                    { "tools", json::makeArray({ toolDescriptor() }) },
                }));
        return true;
    }

    if (method == "tools/call") {
        if (!wantsReply)
            return false;

        const json::Value* params = request.find("params");
        const json::Value* name   = params ? params->find("name") : nullptr;

        if (!name || name->type != json::Value::Type::String) {
            *response = makeError(id, kInvalidParams, "params.name is required");
            return true;
        }
        if (name->string != kToolName) {
            *response = makeError(id, kInvalidParams,
                                  "unknown tool: " + name->string);
            return true;
        }

        // A tool that fails is reported in the result with isError, not as a
        // JSON-RPC error. The distinction matters: a transport error is the
        // client's problem, a tool error is something the model should read and
        // reason about.
        const ScanOutcome outcome = collectProjects();
        if (!outcome.ok) {
            *response = makeResponse(id, textResult(outcome.error, true));
            return true;
        }

        *response = makeResponse(id, textResult(json::dump(outcome.payload), false));
        return true;
    }

    if (!wantsReply)
        return false;

    *response = makeError(id, kMethodNotFound, "unknown method: " + method);
    return true;
}

std::string handleLine(std::string_view line)
{
    json::Value request;
    std::string parseError;

    if (!json::parse(line, &request, &parseError)) {
        const json::Value reply =
            makeError(json::makeNull(), kParseError, "invalid JSON: " + parseError);
        return json::dump(reply);
    }

    // A batch is a JSON array of messages. Supported so a client that sends one
    // is not met with silence, though nothing here produces batched work.
    if (request.type == json::Value::Type::Array) {
        std::vector<json::Value> replies;
        for (const json::Value& item : request.array) {
            json::Value reply;
            if (handleMessage(item, &reply))
                replies.push_back(std::move(reply));
        }
        if (replies.empty())
            return {};
        return json::dump(json::makeArray(std::move(replies)));
    }

    json::Value response;
    if (!handleMessage(request, &response))
        return {};
    return json::dump(response);
}

int run()
{
    // Binary mode on both ends. Text mode would translate the framing newline
    // to CRLF on the way out, and a strict client reading newline-delimited
    // JSON is entitled to object to the stray carriage return.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // stdout carries the protocol and nothing else. Every diagnostic in this
    // program goes to stderr for exactly this reason: one stray printf here
    // corrupts the stream and the client drops the connection.
    std::ios::sync_with_stdio(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        // A client on Windows may send CRLF. The carriage return is not part of
        // the JSON.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line.empty())
            continue;

        const std::string reply = handleLine(line);
        if (reply.empty())
            continue;

        std::fwrite(reply.data(), 1, reply.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    return 0;
}

} // namespace pm::mcp
