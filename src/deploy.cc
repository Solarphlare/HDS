#include "deploy.h"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
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

    const std::string command = "/usr/bin/gh attestation verify " + temp_path + " --repo Solarphlare/Hildabot";
    const int exit_code = std::system(command.c_str());

    if (exit_code != 0) {
        std::cout << "Signature verification failed\n";
        std::filesystem::remove(temp_path);
        req.respond(server::response(400, "Bad Request", "text/plain"));
        req.terminate();
        return;
    }

    std::system("/usr/bin/sudo /usr/bin/systemctl stop hildabot.service");
    // EXDEV with rename, so copy + remove
    std::filesystem::copy(temp_path, "/home/willi/bin/hildabot/hildabot", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(temp_path);
    chmod("/home/willi/bin/hildabot/hildabot", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    std::system("/usr/bin/sudo /usr/bin/systemctl start hildabot.service");

    req.respond(server::response(201, "Deployed", "text/plain"));
    req.terminate();
}
