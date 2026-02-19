#include "deploy.h"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>

#include "response.h"
#include "config.h"

void deploy::verify_and_deploy(server::request& req) {
    if (req.method != server::http_method::POST) {
        std::cout << "Invalid method\n";
        req.respond(server::response(405, "Method Not Allowed", "text/plain"));
        req.terminate();
        return;
    }

    if (req.headers.find("Authorization") == req.headers.end() || req.headers.at("Authorization") != DEPLOY_KEY) {
        std::cout << "Invalid or missing authorization\n";

        req.respond(server::response(401, "Unauthorized", "text/plain"));
        req.terminate();
        return;
    }

    if (!req.multipart_body.has_value()) {
        std::cout << "Missing multipart body\n";
        req.respond(server::response(400, "Bad Request", "text/plain"));
        req.terminate();
        return;
    }

    auto payload = std::find_if(
        req.multipart_body->elements.begin(),
        req.multipart_body->elements.end(),
        [](const server::multipart_element& elem) {
            return elem.name == "payload";
        }
    );

    if (payload == req.multipart_body->elements.end()) {
        std::cout << "Missing payload\n";
        req.respond(server::response(400, "Bad Request", "text/plain"));
        req.terminate();
        return;
    }

    const std::string temp_path = "/tmp/hildabot_pending";
    std::ofstream temp_file(temp_path, std::ios::trunc | std::ios::binary | std::ios::out);
    if (!temp_file.is_open()) {
        std::cout << "Failed to open temp file for writing\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    temp_file.write(reinterpret_cast<const char*>(payload->data.data()), payload->data.size());
    temp_file.close();

    // open socket to /tmp/socket_hds_deployer
    const int deployer_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (deployer_socket < 0) {
        std::cout << "Failed to create socket: " << strerror(errno) << "\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    struct sockaddr_un deployer_addr;
    std::memset(&deployer_addr, 0, sizeof(deployer_addr));
    deployer_addr.sun_family = AF_UNIX;
    std::strncpy(deployer_addr.sun_path, "/tmp/socket_hds_deployer", sizeof(deployer_addr.sun_path) - 1);

    if (connect(deployer_socket, (struct sockaddr*)&deployer_addr, sizeof(deployer_addr)) < 0) {
        std::cout << "Failed to connect to deployer socket: " << strerror(errno) << "\n";
        close(deployer_socket);
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    const int sock_written_bytes = send(deployer_socket, temp_path.c_str(), temp_path.size(), MSG_NOSIGNAL);
    if (sock_written_bytes < 0) {
        std::cout << "Failed to send data to deployer socket: " << strerror(errno) << "\n";
        close(deployer_socket);
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    struct pollfd pfd[1];
    pfd[0].fd = deployer_socket;
    pfd[0].events = POLLIN;

    const int poll_result = poll(pfd, 1, 5000); // 5 second timeout
    if (poll_result <= 0) {
        std::cout << "Timeout or error waiting for deployer response: " << strerror(errno) << "\n";
        close(deployer_socket);
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    char buffer[512];
    const int sock_received_bytes = read(deployer_socket, buffer, sizeof(buffer) - 1);
    if (sock_received_bytes <= 0) {
        std::cout << "Failed to read from deployer socket: " << strerror(errno) << "\n";
        close(deployer_socket);
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    close(deployer_socket);

    buffer[sock_received_bytes] = '\0';
    std::string response(buffer);

    if (response == "ERR_ATTESTATION_VERIFICATION_FAILED") {
        std::cout << "Deployer reported attestation verification failure\n";
        req.respond(server::response(400, "Bad Request", "text/plain"));
        req.terminate();
        return;
    }

    if (response != "SUCCESS") {
        std::cout << "Deployer reported failure: " << response << "\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    req.respond(server::response(201, "Deployed", "text/plain"));
    req.terminate();
}
