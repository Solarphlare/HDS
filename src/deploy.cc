#include "deploy.h"
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
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

    auto signature_file = std::find_if(
        req.multipart_body->elements.begin(),
        req.multipart_body->elements.end(),
        [](const server::multipart_element& elem) {
            return elem.name == "signaturefile";
        }
    );

    if (payload == req.multipart_body->elements.end() || signature_file == req.multipart_body->elements.end()) {
        std::cout << "Missing payload or signaturefile\n";
        req.respond(server::response(400, "Bad Request", "text/plain"));
        req.terminate();
        return;
    }

    FILE* pubkey_file = std::fopen("pubkey.pem", "r");

    if (!pubkey_file) {
        std::cout << "Failed to open pubkey.pem\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();
        return;
    }

    EVP_PKEY* pubkey = PEM_read_PUBKEY(pubkey_file, nullptr, nullptr, nullptr);
    if (!pubkey) {
        std::cout << "Failed to read public key\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();

        std::fclose(pubkey_file);
        return;
    }

    std::fclose(pubkey_file);

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        std::cout << "Failed to create MD context\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();

        EVP_PKEY_free(pubkey);
        return;
    }

    if (EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pubkey) != 1) {
        std::cout << "DigestVerifyInit failed\n";
        req.respond(server::response(500, "Internal Server Error", "text/plain"));
        req.terminate();

        EVP_PKEY_free(pubkey);
        EVP_MD_CTX_free(md_ctx);
        return;
    }

    if (EVP_DigestVerify(md_ctx, signature_file->data.data(), signature_file->data.size(), payload->data.data(), payload->data.size()) <= 0) {
        std::cout << "Signature verification failed\n";
        req.respond(server::response(401, "Unauthorized", "text/plain"));
        req.terminate();

        EVP_PKEY_free(pubkey);
        EVP_MD_CTX_free(md_ctx);
        return;
    }

    std::system("/usr/bin/sudo /usr/bin/systemctl stop hildabot.service");

    std::ofstream outfile("/home/willi/bin/hildabot/hildabot", std::ios::trunc | std::ios::binary | std::ios::out);
    outfile.write(reinterpret_cast<const char*>(payload->data.data()), payload->data.size());
    outfile.close();

    chmod("/home/willi/bin/hildabot/hildabot", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    std::system("/usr/bin/sudo /usr/bin/systemctl start hildabot.service");

    req.respond(server::response(201, "Deployed", "text/plain"));

    EVP_PKEY_free(pubkey);
    EVP_MD_CTX_free(md_ctx);
}
