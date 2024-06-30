#include "signer.hpp"
#include "containers.hpp"
#include "exception.hpp"
#include "ga_strings.hpp"
#include "json_utils.hpp"
#include "memory.hpp"
#include "network_parameters.hpp"
#include "utils.hpp"

namespace green {

    namespace {
        static wally_ext_key_ptr derive(
            const wally_ext_key_ptr& hdkey, uint32_span_t path, uint32_t flags = BIP32_FLAG_KEY_PRIVATE)
        {
            // FIXME: Private keys should be derived into mlocked memory
            return bip32_key_from_parent_path_alloc(hdkey, path, flags | BIP32_FLAG_SKIP_HASH);
        }

        static nlohmann::json get_credentials_json(const nlohmann::json& credentials)
        {
            if (credentials.empty()) {
                // Hardware wallet or remote service
                return {};
            }

            if (auto username = j_str(credentials, "username"); username) {
                // Green old-style watch-only login, or blobserver rich watch-only login
                const auto& password = j_strref(credentials, "password");
                return { { "username", std::move(*username) }, { "password", password } };
            }

            if (auto user_mnemonic = j_str(credentials, "mnemonic"); user_mnemonic) {
                // Mnemonic, or a hex seed
                const auto bip39_passphrase = j_str(credentials, "bip39_passphrase");
                std::string mnemonic = *user_mnemonic;
                if (mnemonic.find(' ') != std::string::npos) {
                    // Mnemonic, possibly encrypted
                    if (auto password = j_str(credentials, "password"); password) {
                        GDK_RUNTIME_ASSERT_MSG(!bip39_passphrase, "cannot use bip39_passphrase and password");
                        // Encrypted; decrypt it
                        mnemonic = decrypt_mnemonic(mnemonic, *password);
                    }
                    auto passphrase = bip39_passphrase.value_or(std::string{});
                    auto seed = b2h(bip39_mnemonic_to_seed(mnemonic, passphrase));
                    nlohmann::json ret = { { "mnemonic", std::move(mnemonic) }, { "seed", std::move(seed) } };
                    if (!passphrase.empty()) {
                        ret["bip39_passphrase"] = std::move(passphrase);
                    }
                    return ret;
                }
                if (mnemonic.size() == 129u && mnemonic.back() == 'X') {
                    // Hex seed (a 512 bit bip32 seed encoding in hex with 'X' appended)
                    GDK_RUNTIME_ASSERT_MSG(!bip39_passphrase, "cannot use bip39_passphrase and hex seed");
                    mnemonic.pop_back();
                    return { { "seed", std::move(mnemonic) } };
                }
            }

            const auto slip132_pubkeys = j_array(credentials, "slip132_extended_pubkeys");
            if (auto descriptors = j_array(credentials, "core_descriptors"); descriptors) {
                // Descriptor watch-only login
                if (slip132_pubkeys) {
                    throw user_error("cannot use slip132_extended_pubkeys and core_descriptors");
                }
                return { { "core_descriptors", std::move(*descriptors) } };
            }

            if (slip132_pubkeys) {
                // Descriptor watch-only login
                return { { "slip132_extended_pubkeys", std::move(*slip132_pubkeys) } };
            }

            throw user_error("Invalid credentials");
        }

        static const nlohmann::json GREEN_DEVICE_JSON{ { "device_type", "green-backend" }, { "supports_low_r", true },
            { "supports_arbitrary_scripts", true }, { "supports_host_unblinding", false },
            { "supports_external_blinding", true }, { "supports_liquid", liquid_support_level::lite },
            { "supports_ae_protocol", ae_protocol_support_level::none } };

        static const nlohmann::json WATCH_ONLY_DEVICE_JSON{ { "device_type", "watch-only" }, { "supports_low_r", true },
            { "supports_arbitrary_scripts", true }, { "supports_host_unblinding", true },
            { "supports_external_blinding", true }, { "supports_liquid", liquid_support_level::lite },
            { "supports_ae_protocol", ae_protocol_support_level::none } };

        static const nlohmann::json SOFTWARE_DEVICE_JSON{ { "device_type", "software" }, { "supports_low_r", true },
            { "supports_arbitrary_scripts", true }, { "supports_host_unblinding", true },
            { "supports_external_blinding", true }, { "supports_liquid", liquid_support_level::lite },
            { "supports_ae_protocol", ae_protocol_support_level::none } };

        static nlohmann::json get_device_json(const nlohmann::json& hw_device, const nlohmann::json& credentials)
        {
            nlohmann::json ret;
            auto device
                = hw_device.empty() ? nlohmann::json::object() : hw_device.value("device", nlohmann::json::object());
            if (!device.empty()) {
                ret.swap(device);
                if (!credentials.empty()) {
                    throw user_error("HWW/remote signer and login credentials cannot be used together");
                }
            } else if (credentials.contains("username") || credentials.contains("slip132_extended_pubkeys")
                || credentials.contains("core_descriptors")) {
                ret = WATCH_ONLY_DEVICE_JSON;
            } else if (credentials.contains("seed")) {
                ret = SOFTWARE_DEVICE_JSON;
            } else {
                throw user_error("Hardware device or credentials required");
            }

            const bool overwrite_null = true;
            json_add_if_missing(ret, "supports_low_r", false, overwrite_null);
            json_add_if_missing(ret, "supports_arbitrary_scripts", false, overwrite_null);
            json_add_if_missing(ret, "supports_host_unblinding", false, overwrite_null);
            json_add_if_missing(ret, "supports_external_blinding", false, overwrite_null);
            json_add_if_missing(ret, "supports_liquid", liquid_support_level::none, overwrite_null);
            json_add_if_missing(ret, "supports_ae_protocol", ae_protocol_support_level::none, overwrite_null);
            json_add_if_missing(ret, "device_type", std::string("hardware"), overwrite_null);
            const auto device_type = j_str_or_empty(ret, "device_type");
            if (device_type == "hardware") {
                if (ret.value("name", std::string()).empty()) {
                    throw user_error("Hardware device JSON requires a non-empty 'name' element");
                }
            } else if (device_type == "green-backend") {
                // Don't allow overriding Green backend settings
                ret = GREEN_DEVICE_JSON;
            } else if (device_type != "software" && device_type != "watch-only") {
                throw user_error(std::string("Unknown device type ") + device_type);
            }
            return ret;
        }
    } // namespace

    const std::array<uint32_t, 0> signer::EMPTY_PATH{};
    const std::array<uint32_t, 1> signer::LOGIN_PATH{ { 0x4741b11e } };
    const std::array<uint32_t, 1> signer::REGISTER_PATH{ { harden(0x4741) } }; // 'GA'
    const std::array<uint32_t, 1> signer::CLIENT_SECRET_PATH{ { harden(0x70617373) } }; // 'pass'
    const std::array<unsigned char, 8> signer::PASSWORD_SALT = {
        { 0x70, 0x61, 0x73, 0x73, 0x73, 0x61, 0x6c, 0x74 } // 'passsalt'
    };
    const std::array<unsigned char, 8> signer::BLOB_SALT = {
        { 0x62, 0x6c, 0x6f, 0x62, 0x73, 0x61, 0x6c, 0x74 } // 'blobsalt'
    };
    const std::array<unsigned char, 8> signer::WATCH_ONLY_SALT = {
        { 0x5f, 0x77, 0x6f, 0x5f, 0x73, 0x61, 0x6c, 0x74 } // '_wo_salt'
    };
    const std::array<unsigned char, 8> signer::WO_SEED_U = {
        { 0x01, 0x77, 0x6f, 0x5f, 0x75, 0x73, 0x65, 0x72 } // [1]'wo_user'
    };
    const std::array<unsigned char, 8> signer::WO_SEED_P = {
        { 0x02, 0x77, 0x6f, 0x5f, 0x70, 0x61, 0x73, 0x73 } // [2]'wo_pass'
    };
    const std::array<unsigned char, 8> signer::WO_SEED_K = {
        { 0x03, 0x77, 0x6f, 0x5f, 0x62, 0x6C, 0x6f, 0x62 } // [3]'wo_blob'
    };

    signer::signer(
        const network_parameters& net_params, const nlohmann::json& hw_device, const nlohmann::json& credentials)
        : m_is_main_net(net_params.is_main_net())
        , m_is_liquid(net_params.is_liquid())
        , m_btc_version(net_params.btc_version())
        , m_credentials(get_credentials_json(credentials))
        , m_device(get_device_json(hw_device, m_credentials))
    {
        if (m_is_liquid && get_liquid_support() == liquid_support_level::none) {
            throw user_error(res::id_the_hardware_wallet_you_are);
        }

        if (const auto seed_hex = j_str(m_credentials, "seed"); seed_hex) {
            // FIXME: Allocate m_master_key in mlocked memory
            std::vector<unsigned char> seed = h2b(*seed_hex);
            const uint32_t version = m_is_main_net ? BIP32_VER_MAIN_PRIVATE : BIP32_VER_TEST_PRIVATE;
            m_master_key = bip32_key_from_seed_alloc(seed, version, 0);
            if (m_is_liquid) {
                m_master_blinding_key = asset_blinding_key_from_seed(seed);
            }
            bzero_and_free(seed);
        }
    }

    signer::~signer()
    {
        if (m_master_blinding_key) {
            wally_bzero(m_master_blinding_key->data(), m_master_blinding_key->size());
        }
    }

    bool signer::is_compatible_with(const std::shared_ptr<signer>& other) const
    {
        if (get_device() != other->get_device()) {
            return false;
        }
        auto my_credentials = get_credentials();
        my_credentials.erase("master_blinding_key");
        auto other_credentials = other->get_credentials();
        other_credentials.erase("master_blinding_key");
        return my_credentials == other_credentials;
    }

    std::string signer::get_mnemonic(const std::string& password)
    {
        if (is_hardware() || is_watch_only() || is_remote()) {
            return std::string();
        }
        if (const auto mnemonic = j_str(m_credentials, "mnemonic"); mnemonic) {
            return encrypt_mnemonic(*mnemonic, password); // Mnemonic
        }
        return j_strref(m_credentials, "seed") + "X"; // Hex seed
    }

    bool signer::supports_low_r() const
    {
        // Note we always use AE if the HW supports it
        return !use_ae_protocol() && j_boolref(m_device, "supports_low_r");
    }

    bool signer::supports_arbitrary_scripts() const { return j_boolref(m_device, "supports_arbitrary_scripts"); }

    liquid_support_level signer::get_liquid_support() const { return m_device.at("supports_liquid"); }

    bool signer::supports_host_unblinding() const { return j_boolref(m_device, "supports_host_unblinding"); }

    bool signer::supports_external_blinding() const { return j_boolref(m_device, "supports_external_blinding"); }

    ae_protocol_support_level signer::get_ae_protocol_support() const { return m_device.at("supports_ae_protocol"); }

    bool signer::use_ae_protocol() const { return get_ae_protocol_support() != ae_protocol_support_level::none; }

    bool signer::is_remote() const { return j_strref(m_device, "device_type") == "green-backend"; }

    bool signer::is_liquid() const { return m_is_liquid; }

    bool signer::is_watch_only() const { return j_strref(m_device, "device_type") == "watch-only"; }

    bool signer::is_hardware() const { return j_strref(m_device, "device_type") == "hardware"; }

    bool signer::is_descriptor_watch_only() const
    {
        return m_credentials.contains("core_descriptors") || m_credentials.contains("slip132_extended_pubkeys");
    }

    const nlohmann::json& signer::get_device() const { return m_device; }

    nlohmann::json signer::get_credentials() const
    {
        auto credentials = m_credentials;
        if (m_is_liquid) {
            // Return the master blinding key if we have one
            std::unique_lock<std::mutex> locker{ m_mutex };
            if (m_master_blinding_key.has_value()) {
                auto key = gsl::make_span(m_master_blinding_key.value());
                credentials["master_blinding_key"] = b2h(key.last(HMAC_SHA256_LEN));
            }
        }
        return credentials;
    }

    std::string signer::get_master_bip32_xpub() { return get_bip32_xpub({}); }

    bool signer::has_master_bip32_xpub() { return has_bip32_xpub({}); }

    xpub_t signer::get_master_xpub() { return get_xpub({}); }

    std::string signer::get_bip32_xpub(const std::vector<uint32_t>& path)
    {
        std::vector<uint32_t> parent_path{ path }, child_path;
        child_path.reserve(path.size());
        wally_ext_key_ptr parent_key;

        {
            // Search for the cached xpub or a parent we can derive it from
            std::unique_lock<std::mutex> locker{ m_mutex };
            for (;;) {
                auto cached = m_cached_bip32_xpubs.find(parent_path);
                if (cached != m_cached_bip32_xpubs.end()) {
                    if (child_path.empty()) {
                        // Found the full derived key, return it
                        return cached->second;
                    }
                    // Found a parent of the key we are looking for
                    parent_key = bip32_public_key_from_bip32_xpub(cached->second);
                    break;
                }
                if (parent_path.empty() || is_hardened(parent_path.back())) {
                    // Root key or hardened parent we don't have yet: try below
                    break;
                }
                // Try the next highest possible parent
                child_path.insert(child_path.begin(), parent_path.back());
                parent_path.pop_back();
            }
        }
        if (path.empty()) {
            // Master xpub requested. encache and return it
            return cache_ext_key({}, m_master_key);
        }
        if (!parent_path.empty() && !parent_key) {
            // Derive and encache the parent key from the master key
            GDK_RUNTIME_ASSERT(m_master_key);
            parent_key = derive(m_master_key, parent_path, BIP32_FLAG_KEY_PUBLIC);
            cache_ext_key(parent_path, parent_key);
        }
        auto& root_key = parent_key ? parent_key : m_master_key;
        GDK_RUNTIME_ASSERT(root_key);
        if (child_path.empty()) {
            // Return our root key, which is already cached
            const auto key_data = bip32_key_serialize(*root_key, BIP32_FLAG_KEY_PUBLIC);
            return base58check_from_bytes(key_data);
        }
        // Derive, encache and return the child key from the root key
        auto child_key = derive(root_key, child_path, BIP32_FLAG_KEY_PUBLIC);
        return cache_ext_key(path, child_key); // Cache with the full path
    }

    bool signer::has_bip32_xpub(const std::vector<uint32_t>& path)
    {
        if (m_master_key) {
            return true; // We can derive any xpub we need
        }
        std::vector<uint32_t> parent_path{ path };
        std::unique_lock<std::mutex> locker{ m_mutex };
        for (;;) {
            auto cached = m_cached_bip32_xpubs.find(parent_path);
            if (cached != m_cached_bip32_xpubs.end()) {
                return true; // Found
            }
            if (parent_path.empty() || is_hardened(parent_path.back())) {
                // Root key or hardened parent we don't have
                return false;
            }
            // Try the next highest possible parent
            parent_path.pop_back();
        }
    }

    xpub_t signer::get_xpub(const std::vector<uint32_t>& path) { return make_xpub(get_bip32_xpub(path)); }

    std::string signer::cache_ext_key(const std::vector<uint32_t>& path, const wally_ext_key_ptr& hdkey)
    {
        // Encache the derived key with the full path
        GDK_RUNTIME_ASSERT(hdkey);
        const auto key_data = bip32_key_serialize(*hdkey, BIP32_FLAG_KEY_PUBLIC);
        auto xpub = base58check_from_bytes(key_data);
        cache_bip32_xpub(path, xpub);
        return xpub;
    }

    bool signer::cache_bip32_xpub(const std::vector<uint32_t>& path, const std::string& bip32_xpub)
    {
        std::unique_lock<std::mutex> locker{ m_mutex };
        auto ret = m_cached_bip32_xpubs.emplace(path, bip32_xpub);
        // If already present, verify that the value matches
        GDK_RUNTIME_ASSERT(ret.second || ret.first->second == bip32_xpub);
        return ret.second; // Return whether or not the xpub was already present
    }

    signer::cache_t signer::get_cached_bip32_xpubs()
    {
        std::unique_lock<std::mutex> locker{ m_mutex };
        return m_cached_bip32_xpubs;
    }

    nlohmann::json signer::get_cached_bip32_xpubs_json()
    {
        auto paths_and_xpubs = get_cached_bip32_xpubs();
        nlohmann::json xpubs_json;
        for (auto& item : paths_and_xpubs) {
            // We cache the values inverted, i.e. xpub: path
            // because the master key path is empty JSON keys can't be empty
            xpubs_json.emplace(std::move(item.second), std::move(item.first));
        }
        return xpubs_json;
    }

    ecdsa_sig_t signer::sign_hash(uint32_span_t path, byte_span_t hash)
    {
        GDK_RUNTIME_ASSERT(m_master_key);
        wally_ext_key_ptr derived = derive(m_master_key, path);
        return ec_sig_from_bytes(gsl::make_span(derived->priv_key).subspan(1), hash);
    }

    ecdsa_sig_rec_t signer::sign_rec_hash(uint32_span_t path, byte_span_t hash)
    {
        GDK_RUNTIME_ASSERT(m_master_key);
        wally_ext_key_ptr derived = derive(m_master_key, path);
        return ec_sig_rec_from_bytes(gsl::make_span(derived->priv_key).subspan(1), hash);
    }

    bool signer::has_master_blinding_key() const
    {
        std::unique_lock<std::mutex> locker{ m_mutex };
        return m_master_blinding_key.has_value();
    }

    blinding_key_t signer::get_master_blinding_key() const
    {
        std::unique_lock<std::mutex> locker{ m_mutex };
        GDK_RUNTIME_ASSERT(m_master_blinding_key.has_value());
        return m_master_blinding_key.value();
    }

    void signer::set_master_blinding_key(const std::string& blinding_key_hex)
    {
        if (!blinding_key_hex.empty()) {
            const auto key_bytes = h2b(blinding_key_hex);
            const auto key_size = key_bytes.size();
            GDK_RUNTIME_ASSERT(key_size == SHA512_LEN || key_size == SHA512_LEN / 2);
            blinding_key_t key{ 0 };
            // Handle both full and half-size blinding keys
            std::copy(key_bytes.begin(), key_bytes.end(), key.begin() + (SHA512_LEN - key_size));
            std::unique_lock<std::mutex> locker{ m_mutex };
            m_master_blinding_key = key;
        }
    }

    priv_key_t signer::get_blinding_key_from_script(byte_span_t script)
    {
        std::unique_lock<std::mutex> locker{ m_mutex };
        GDK_RUNTIME_ASSERT(m_master_blinding_key.has_value());
        return asset_blinding_key_to_ec_private_key(*m_master_blinding_key, script);
    }

    std::vector<unsigned char> signer::get_blinding_pubkey_from_script(byte_span_t script)
    {
        return ec_public_key_from_private_key(get_blinding_key_from_script(script));
    }

} // namespace green
