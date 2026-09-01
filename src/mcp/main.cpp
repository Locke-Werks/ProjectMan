#include "server.h"

#include <cstdio>
#include <cstring>

// ProjectMan MCP: an stdio Model Context Protocol server over the same index
// ProjectMan builds, exposing exactly one read-only tool.
//
// It reports. It does not launch anything, write anything, dispatch anything or
// create a config. That is the entire product, and the narrowness is the point:
// an MCP server wired into an agent is a standing capability, so the smallest
// honest surface is the right one.
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0) {
            std::printf("ProjectMan MCP %s\n", PM_VERSION_STRING);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fputs(
                "ProjectMan MCP " PM_VERSION_STRING "\n"
                "\n"
                "An stdio MCP server exposing one read-only tool, get-projects,\n"
                "which reports every project under the ProjectMan projects root\n"
                "with its git state and outstanding work.\n"
                "\n"
                "It speaks JSON-RPC on stdin and stdout and is meant to be run by\n"
                "an MCP client, not by hand. To register it with Claude Code:\n"
                "\n"
                "  claude mcp add projectman -- pm-mcp\n"
                "\n"
                "or, when the install directory is not on PATH:\n"
                "\n"
                "  claude mcp add projectman -- \\\n"
                "    \"C:\\Program Files\\Locke Werks\\ProjectMan MCP\\pm-mcp.exe\"\n"
                "\n"
                "Requires ProjectMan, whose config it reads and never creates.\n",
                stderr);
            return 0;
        }
    }

    return pm::mcp::run();
}
