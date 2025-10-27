#include "response.h"
#include <cstdint>

server::response::response(int status, const std::string& body, const std::string& content_type) : body(body.begin(), body.end()), status_code(status) {
    headers["Content-Length"] = std::to_string(body.size());
    headers["Content-Type"] = content_type;
    headers["Connection"] = "close";
    headers["Server"] = "HDS/1.0.1";

    this->body = std::vector<uint8_t>(body.begin(), body.end());
}

server::response::response(int status, const std::vector<uint8_t>& body, const std::string& content_type) : body(body), status_code(status) {
    headers["Content-Length"] = std::to_string(body.size());
    headers["Content-Type"] = content_type;
    headers["Connection"] = "close";
    headers["Server"] = "HDS/1.0.1";
}

void server::response::set_header(const std::string& key, const std::string& value) {
    headers[key] = value;
}
