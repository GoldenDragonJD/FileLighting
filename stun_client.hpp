#pragma once

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <asio.hpp>
#include <iostream>
#include <vector>
#include <array>
#include <random>
#include <string>
#include <optional>

namespace stun {

// Struct representing the fixed 20-byte STUN header
struct StunHeader {
    uint16_t type;
    uint16_t length;
    uint32_t magic_cookie;
    uint8_t  transaction_id[12];
};

// Struct to hold the public IP and port
struct PublicEndpoint {
    std::string ip;
    uint16_t port;
};

// Utility function to fetch the public IP and port from STUN servers
inline std::optional<PublicEndpoint> fetch_public_endpoint(asio::io_context& io_ctx) {
    using asio::ip::udp;

    std::vector<std::pair<std::string, std::string>> stun_servers = {
        {"stun.l.google.com", "19302"},
        {"stun1.l.google.com", "19302"},
        {"stun.cloudflare.com", "3478"} // Excellent secondary fallback
    };

    for (const auto& [host, server_port] : stun_servers) {
        try {
            // 1. Resolve STUN server endpoint
            udp::resolver resolver(io_ctx);
            auto endpoints = resolver.resolve(udp::v4(), host, server_port);
            if (endpoints.empty()) continue;
            
            udp::endpoint stun_endpoint = *endpoints.begin();

            // 2. Open an ephemeral UDP socket
            udp::socket sock(io_ctx, udp::endpoint(udp::v4(), 0));

            // 3. Construct the 20-byte STUN Binding Request packet manually
            std::vector<uint8_t> request(20, 0);
            
            // Message Type: 0x0001 (Binding Request) in network byte order (Big Endian)
            request[0] = 0x00; request[1] = 0x01;
            // Message Length: 0x0000 (No attributes)
            request[2] = 0x00; request[3] = 0x00;
            // Magic Cookie: 0x2112A442 (Big Endian)
            request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;

            // Transaction ID: Fill with 12 random bytes
            std::random_device rd;
            for (size_t i = 8; i < 20; ++i) {
                request[i] = static_cast<uint8_t>(rd() % 256);
            }

            // 4. Send the request
            sock.send_to(asio::buffer(request), stun_endpoint);

            // 5. Read the response packet
            std::array<uint8_t, 1024> recv_buffer;
            udp::endpoint sender_endpoint;
            asio::error_code ec;
            size_t len = sock.receive_from(asio::buffer(recv_buffer), sender_endpoint, 0, ec);

            if (ec || len < 20) {
                continue; // Move to the next server on error or invalid packet size
            }

            // 6. Parse the STUN attributes to locate the XOR-MAPPED-ADDRESS (Type: 0x0020)
            size_t index = 20;
            while (index + 4 <= len) {
                uint16_t attr_type = (recv_buffer[index] << 8) | recv_buffer[index + 1];
                uint16_t attr_len  = (recv_buffer[index + 2] << 8) | recv_buffer[index + 3];
                index += 4;

                // Check if this attribute is the XOR-MAPPED-ADDRESS (0x0020)
                if (attr_type == 0x0020 && index + attr_len <= len) {
                    // Byte 1: Protocol Family (0x01 for IPv4, 0x02 for IPv6)
                    uint8_t family = recv_buffer[index + 1];
                    
                    if (family == 0x01) { // IPv4 Handling
                        // The port is obfuscated by XORing it with the top 16 bits of the Magic Cookie
                        uint16_t xor_port = (recv_buffer[index + 2] << 8) | recv_buffer[index + 3];
                        uint16_t actual_port = xor_port ^ 0x2112;

                        // The IP address is obfuscated by XORing it directly with the 32-bit Magic Cookie
                        uint32_t actual_ip = 0;
                        actual_ip |= (recv_buffer[index + 4] ^ 0x21) << 24;
                        actual_ip |= (recv_buffer[index + 5] ^ 0x12) << 16;
                        actual_ip |= (recv_buffer[index + 6] ^ 0xA4) << 8;
                        actual_ip |= (recv_buffer[index + 7] ^ 0x42);

                        // Convert to human-readable string notation
                        std::string ip_str = std::to_string((actual_ip >> 24) & 0xFF) + "." +
                                             std::to_string((actual_ip >> 16) & 0xFF) + "." +
                                             std::to_string((actual_ip >> 8) & 0xFF) + "." +
                                             std::to_string(actual_ip & 0xFF);

                        return PublicEndpoint{ip_str, actual_port}; // Success!
                    }
                }
                // STUN attributes are padded to multiples of 4 bytes
                index += (attr_len + 3) & ~3;
            }
        } catch (const std::exception& e) {
            std::cerr << "[STUN] Error with " << host << ": " << e.what() << "\n";
            // Continue to the next server on exception
        }
    }

    return std::nullopt; // All servers failed
}

} // namespace stun
