#include "multipart.h"
#include <string>
#include <stdexcept>
#include <algorithm>
#include <ranges>
#include <cstring>
#include <unordered_map>
#include <iterator>

const uint8_t* search_bytes(const uint8_t* begin, const uint8_t* end, const std::string& needle) {
    auto it = std::search(begin, end, needle.begin(), needle.end());
    return it == end ? nullptr : it;
}

server::multipart_body::multipart_body(std::vector<uint8_t>& body, std::unordered_map<std::string, std::string>& req_headers) {
    auto it = req_headers.find("Content-Type");
    if (it == req_headers.end()) {
        throw std::runtime_error("Content-Type header not found");
    }

    const std::string& content_type = it->second;
    const std::string boundary_prefix = "boundary=";
    size_t boundary_pos = content_type.find(boundary_prefix);
    if (boundary_pos == std::string::npos) {
        throw std::runtime_error("Boundary not found in Content-Type header");
    }

    this->boundary = "--" + content_type.substr(boundary_pos + boundary_prefix.length());

    if (this->boundary.empty()) {
        throw std::runtime_error("Boundary is empty");
    }

    const uint8_t* begin = body.data();
    const uint8_t* end = body.data() + body.size();

    auto cur = search_bytes(begin, end, this->boundary);
    if (cur == end) {
        throw std::runtime_error("Boundary not found in body");
    }

    cur += this->boundary.size();

    // After boundary must be CRLF or "--" (closing)
    if (cur + 1 < end && cur[0] == '-' && cur[1] == '-') {
        // Empty multipart
        return;
    }

    if (cur + 1 >= end || cur[0] != '\r' || cur[1] != '\n') {
        throw std::runtime_error("Malformed boundary line");
    }
    cur += 2; // skip CRLF

    const std::string header_sep = "\r\n\r\n";
    const std::string crlf = "\r\n";
    const std::string next_boundary = "\r\n" + this->boundary;
    const std::string final_suffix = "--";

    while (cur < end) {
        // Parse headers
        const uint8_t* hdr_end = search_bytes(cur, end, header_sep);
        if (!hdr_end) {
            throw std::runtime_error("Multipart headers not terminated");
        }

        // Split headers by CRLF
        std::string name;
        std::optional<std::string> filename = std::nullopt;
        std::string content_type_header = "text/plain";

        const uint8_t* line_start = cur;
        while (line_start < hdr_end) {
            const uint8_t* line_end = search_bytes(line_start, hdr_end, crlf);
            if (!line_end) line_end = hdr_end;
            std::string line(reinterpret_cast<const char*>(line_start), line_end - line_start);

            if (line.rfind("Content-Disposition: ", 0) == 0) {
                // Parse name and filename
                std::size_t name_it = line.find("name=\"");

                if (name_it != std::string::npos) {
                    name_it += 6;
                    std::size_t name_end = line.find('"', name_it);

                    if (name_end != std::string::npos) {
                        name = line.substr(name_it, name_end - name_it);
                    }
                }

                std::size_t fn_it = line.find("filename=\"");

                if (fn_it != std::string::npos) {
                    fn_it += 10;
                    std::size_t fn_end = line.find('"', fn_it);

                    if (fn_end != std::string::npos) {
                        filename = line.substr(fn_it, fn_end - fn_it);
                    }
                }
            }
            else if (line.rfind("Content-Type: ", 0) == 0) {
                content_type_header = line.substr(14);
            }

            line_start = (line_end < hdr_end) ? (line_end + crlf.size()) : hdr_end;
        }

        const uint8_t* data_start = hdr_end + header_sep.size();

        // Find next boundary
        const uint8_t* marker = search_bytes(data_start, end, next_boundary);
        if (!marker) {
            // Try final boundary without leading CRLF (end of stream)
            marker = search_bytes(data_start, end, this->boundary);

            if (!marker) {
                throw std::runtime_error("Next boundary not found");
            }
            if (marker != data_start && (marker[-2] != '\r' || marker[-1] != '\n')) {
                // Must be preceded by CRLF
                throw std::runtime_error("Boundary not preceded by CRLF");
            }

            marker -= 2; // exclude preceding CRLF from data
        }

        // Exclude the CRLF that precedes the boundary from the part data
        const uint8_t* data_end = marker;
        if (data_end >= data_start + 2 && data_end[-2] == '\r' && data_end[-1] == '\n') {
            data_end -= 2;
        }

        // Store element
        if (!name.empty()) {
            std::vector<uint8_t> data(data_start, data_end);
            this->elements.emplace_back(std::move(name), std::move(filename), std::move(content_type_header), std::move(data));
        }

        // Advance past boundary line
        const uint8_t* b = (marker == data_end) ? (marker + 2) : (data_end + 2); // position at CRLF before boundary
        const uint8_t* after = b;

        if (after + next_boundary.size() <= end && std::equal(next_boundary.begin(), next_boundary.end(), after)) {
            after += next_boundary.size();
        }
        else {
            // We already validated boundary_marker without CRLF case
            after = search_bytes(after, end, this->boundary);
            if (!after) break;
            after += this->boundary.size();
        }

        // Check for closing
        if (after + 1 < end && after[0] == '-' && after[1] == '-') {
            break;
        }

        if (after + 1 >= end || after[0] != '\r' || after[1] != '\n') {
            throw std::runtime_error("Malformed boundary continuation");
        }
        cur = after + 2; // next part
    }
}
