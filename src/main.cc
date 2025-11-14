#include <iostream>
#include <stdexcept>
#include <thread>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>
#include "request.h"
#include "deploy.h"

int socket_fd;
struct sockaddr_in6 server_addr;

void initialize_socket() {
    if ((socket_fd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        throw std::runtime_error("Socket creation failed");
    }

    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(8443);

    const int opt_false = false;
    // const int opt_true = true;
    if (setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt_false, sizeof(opt_false)) < 0) {
        throw std::runtime_error("setsockopt failed");
    }

    // if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt_true, sizeof(opt_true)) < 0) {
    //     throw std::runtime_error("setsockopt (2) failed");
    // }

    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        throw std::runtime_error("Bind failed");
    }

    if (listen(socket_fd, 5) < 0) {
        throw std::runtime_error("Listen failed");
    }
}

void handle_client(SSL_CTX* ctx, int client_fd, const sockaddr_in6 client_addr) {
    server::request request;

    try {
        request = server::request(ctx, client_fd);
    }
    catch (const std::exception& e) {
        return;
    }

    char client_ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &client_addr.sin6_addr, client_ip, sizeof(client_ip));
    std::cout << (request.method == server::http_method::POST ? "POST" : "OTHER") << " " << request.path << " from " << client_ip << '\n';

    if (request.path != "/hildabot/deploy") {
        server::response response(404, "Not Found", "text/plain");
        request.respond(response);
        request.terminate();
        return;
    }
    else {
        deploy::verify_and_deploy(request);
        return;
    }
}

int main() {
    OPENSSL_init_ssl(0, nullptr);
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    OPENSSL_init_ssl(OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS, nullptr);
    initialize_socket();

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!ctx) {
        throw std::runtime_error("Unable to create SSL context");
    }

    if (SSL_CTX_use_certificate_file(ctx.get(), "cert.pem", SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("Unable to load certificate file");
    if (SSL_CTX_use_PrivateKey_file(ctx.get(), "key.pem", SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("Unable to load private key file");

    std::cout << "Server started; listening on port " << ntohs(server_addr.sin6_port) << '\n';

    while (true) {
        struct sockaddr_in6 client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = 0;

        if ((client_fd = accept(socket_fd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
            continue;
        }

        std::thread client_thread(handle_client, ctx.get(), client_fd, client_addr);
        client_thread.detach();
    }

    close(socket_fd);
}

