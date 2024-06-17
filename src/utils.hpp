#ifndef GDK_UTILS_HPP
#define GDK_UTILS_HPP
#pragma once

#include <cstddef>
#include <string>

#include "ga_wally.hpp"
#include "gdk.h"
#include "json_utils.hpp"
#include "logging.hpp"

namespace green {

    class network_parameters;

    void get_random_bytes(std::size_t num_bytes, void* output_bytes, std::size_t siz);

    template <std::size_t N> std::array<unsigned char, N> get_random_bytes()
    {
        std::array<unsigned char, N> buff{ { 0 } };
        get_random_bytes(N, buff.data(), buff.size());
        return buff;
    }

    // Return a uint32_t in the range 0 to (upper_bound - 1) without bias
    uint32_t get_uniform_uint32_t(uint32_t upper_bound);

    // STL compatible RNG returning uniform uint32_t's
    struct uniform_uint32_rng {
        uniform_uint32_rng() // NOLINT: ignored for valgrind use
            : m_index(std::tuple_size<decltype(m_entropy)>::value - 1u)
        {
        }

        using result_type = uint32_t;
        constexpr static result_type min() { return std::numeric_limits<result_type>::min(); }
        constexpr static result_type max() { return std::numeric_limits<result_type>::max(); }
        result_type operator()();

    private:
        std::array<result_type, 8> m_entropy; // NOLINT: ignored for valgrind use
        size_t m_index;
    };

    // example: [0,1,2,3] --> binary_op(0,1), binary_op(2,3)
    template <typename InputIt, typename OutputIt, typename BinaryOperation>
    void pair_and_transform(InputIt first, InputIt last, OutputIt d_first, BinaryOperation binary_op)
    {
        auto next = first;
        while (next != last) {
            auto prev = next++;
            *d_first++ = binary_op(*prev, *next++);
        }
    }

    bool nsee_log_info(std::string message, const char* context);
    std::string get_diagnostic_information(const boost::exception& e);

    template <typename F> bool no_std_exception_escape(F&& fn, const char* context = "") noexcept
    {
        std::string message;
        try {
            fn();
            return false;
        } catch (const boost::exception& e) {
            try {
                message = get_diagnostic_information(e);
            } catch (const std::exception&) {
            }
        } catch (const std::exception& e) {
            try {
                message = e.what();
            } catch (const std::exception&) {
            }
        }
        return nsee_log_info(message, context);
    }

    // Parse a BIP-21 style URI into its components
    // If the uri passed is not a bitcoin uri returns an empty json object
    nlohmann::json parse_bitcoin_uri(const network_parameters& net_params, const std::string& uri);

    nlohmann::json parse_url(const std::string& endpoint);
    nlohmann::json select_url(const std::vector<nlohmann::json>& urls, bool use_tor);
    std::string socksify(const std::string& proxy);
    std::string unsocksify(const std::string& proxy);

    std::string format_recovery_key_message(const std::string& xpub, uint32_t subaccount, uint32_t version = 0);

    // Mnemonic handling
    std::string encrypt_mnemonic(const std::string& plaintext_mnemonic, const std::string& password);
    std::string decrypt_mnemonic(const std::string& encrypted_mnemonic, const std::string& password);

    // Compute base entropy for a client blob watch only login
    std::vector<unsigned char> get_wo_entropy(const std::string& username, const std::string& password);

    // Compute username and password for a client blob watch only login
    std::pair<std::string, std::string> get_wo_credentials(byte_span_t entropy);

    // Compute a local cache password for a client blob watch only login
    pub_key_t get_wo_local_encryption_key(byte_span_t entropy, const std::string& server_entropy);

    // Encrypt the client blob key to the watch only entropy, return as hex
    std::string encrypt_wo_blob_key(byte_span_t entropy, const pbkdf2_hmac256_t& blob_key);

    // Decrypt the encrypted client blob key with the watch only entropy
    pbkdf2_hmac256_t decrypt_wo_blob_key(byte_span_t entropy, const std::string& wo_blob_key_hex);

    // Encryption
    std::vector<unsigned char> aes_cbc_decrypt(const pbkdf2_hmac256_t& key, byte_span_t ciphertext);
    std::vector<unsigned char> aes_cbc_decrypt_from_hex(const pbkdf2_hmac256_t& key, const std::string& ciphertext_hex);
    std::vector<unsigned char> aes_cbc_encrypt(const pbkdf2_hmac256_t& key, byte_span_t plaintext);
    std::string aes_cbc_encrypt_to_hex(const pbkdf2_hmac256_t& key, byte_span_t plaintext);

    size_t aes_gcm_decrypt_get_length(byte_span_t cyphertext);
    size_t aes_gcm_decrypt(byte_span_t key, byte_span_t cyphertext, gsl::span<unsigned char> plaintext);
    size_t aes_gcm_encrypt_get_length(byte_span_t plaintext);
    size_t aes_gcm_encrypt(byte_span_t key, byte_span_t plaintext, gsl::span<unsigned char> cyphertext);

    // Return prefix followed by compressed `bytes`
    std::vector<unsigned char> compress(byte_span_t prefix, byte_span_t bytes);
    // Return decompressed `bytes` (prefix is assumed removed by the caller)
    std::vector<unsigned char> decompress(byte_span_t bytes);

    std::string get_wallet_hash_id(const std::string& chain_code_hex, const std::string& public_key_hex,
        bool is_mainnet, const std::string& network);
    nlohmann::json get_wallet_hash_ids(
        const network_parameters& net_params, const std::string& chain_code_hex, const std::string& public_key_hex);
    nlohmann::json get_wallet_hash_ids(const nlohmann::json& net_params, const nlohmann::json& params);

    // RUST FFI:
    // GA_init for rust
    void init_rust(const nlohmann::json& details);

    // Make a call into rust code and return the result
    nlohmann::json rust_call(const std::string& method, const nlohmann::json& details, void* session = nullptr);

    // Return the SPV verification status of a tx
    uint32_t spv_verify_tx(const nlohmann::json& details);

    // Convert an SPV status into one of:
    // "in_progress", "verified", "not_verified", "disabled", "not_longest", "unconfirmed"
    std::string spv_get_status_string(uint32_t spv_status);

    std::string gdb_dump_json(const nlohmann::json& json);

    // Check if str represents a valid utf-8 string
    bool is_valid_utf8(const std::string& str);

    // Set an optional variable, which must be empty or have the same value
    template <typename T> static bool set_optional_variable(std::optional<T>& var, T&& new_value)
    {
        // Allow changing the value only if it is not already set
        GDK_RUNTIME_ASSERT(!var.has_value() || var == new_value);
        if (!var.has_value()) {
            var.emplace(std::move(new_value));
            return true;
        }
        return false;
    }

} // namespace green

#endif
