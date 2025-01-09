#ifndef GDK_SESSION_IMPL_HPP
#define GDK_SESSION_IMPL_HPP
#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

#include "amount.hpp"
#include "ga_wally.hpp"
#include "io_runner.hpp"
#include "network_parameters.hpp"

namespace green {

    using pubkey_and_script_t = std::pair<std::vector<unsigned char>, std::vector<unsigned char>>;
    using unique_pubkeys_and_scripts_t = std::set<pubkey_and_script_t>;

    class client_blob;
    class green_pubkeys;
    class user_pubkeys;
    class green_recovery_pubkeys;
    class signer;
    class Tx;
    struct tor_controller;
    class wamp_transport;
    class xpub_hdkey;

    class session_impl {
    public:
        using locker_t = std::unique_lock<std::mutex>;

        explicit session_impl(network_parameters&& net_params);
        session_impl(const session_impl& other) = delete;
        session_impl(session_impl&& other) noexcept = delete;
        session_impl& operator=(const session_impl& other) = delete;
        session_impl& operator=(session_impl&& other) noexcept = delete;

        virtual ~session_impl();

        // Factory method
        static std::shared_ptr<session_impl> create(const nlohmann::json& net_params);

        // UTXOs
        using utxo_cache_value_t = std::shared_ptr<const nlohmann::json>;

        // Lookup cached UTXOs
        utxo_cache_value_t get_cached_utxos(uint32_t subaccount, uint32_t num_confs) const;
        // Encache UTXOs. Takes ownership of utxos, returns the encached value
        utxo_cache_value_t set_cached_utxos(uint32_t subaccount, uint32_t num_confs, nlohmann::json& utxos);
        // Un-encache UTXOs
        void remove_cached_utxos(const std::vector<uint32_t>& subaccounts);

        virtual nlohmann::json get_unspent_outputs(const nlohmann::json& details, unique_pubkeys_and_scripts_t& missing)
            = 0;
        virtual void process_unspent_outputs(nlohmann::json& utxos);
        nlohmann::json get_external_unspent_outputs(const nlohmann::json& details);
        virtual nlohmann::json set_unspent_outputs_status(
            const nlohmann::json& details, const nlohmann::json& twofactor_data)
            = 0;

        virtual nlohmann::json register_user(std::shared_ptr<signer> signer);

        // Disable notifications from being delivered
        void disable_notifications();
        // Call the users registered notification handler. Must be called without any locks held.
        virtual void emit_notification(nlohmann::json details, bool async);
        std::string connect_tor();
        void reconnect();
        void reconnect_hint(const nlohmann::json& hint);
        virtual void reconnect_hint_session(const nlohmann::json& hint, const nlohmann::json& proxy) = 0;
        // Get the tor or user connection proxy address
        nlohmann::json get_proxy_settings();
        nlohmann::json get_net_call_params(uint32_t timeout_secs);
        nlohmann::json get_registry_config();

        void connect();
        virtual void connect_session();
        void disconnect();
        virtual void disconnect_session();

        // Make an http request to an arbitrary host governed by 'params'.
        virtual nlohmann::json http_request(nlohmann::json params);
        virtual void refresh_assets(nlohmann::json params);
        nlohmann::json get_assets(nlohmann::json params);
        virtual nlohmann::json validate_asset_domain_name(const nlohmann::json& params);

        virtual void start_sync_threads();
        virtual std::vector<uint32_t> get_subaccount_pointers() = 0;
        virtual std::string get_challenge(const pub_key_t& public_key) = 0;
        virtual nlohmann::json authenticate(const std::string& sig_der_hex, std::shared_ptr<signer> signer) = 0;
        virtual void register_subaccount_xpubs(
            const std::vector<uint32_t>& pointers, const std::vector<std::string>& bip32_xpubs)
            = 0;
        virtual nlohmann::json credentials_from_pin_data(const nlohmann::json& pin_data) = 0;
        virtual nlohmann::json login_wo(std::shared_ptr<signer> signer) = 0;
        virtual nlohmann::json set_wo_credentials(const nlohmann::json& credentials);
        virtual std::string get_watch_only_username();
        pub_key_t set_blob_key_from_credentials(locker_t& locker);
        virtual bool remove_account(const nlohmann::json& twofactor_data) = 0;

        // Returns true if the subaccount was discovered
        virtual bool discover_subaccount(uint32_t subaccount, const std::string& xpub, const std::string& sa_type);
        virtual uint32_t get_next_subaccount(const std::string& sa_type) = 0;
        virtual uint32_t get_last_empty_subaccount(const std::string& sa_type);
        virtual nlohmann::json create_subaccount(nlohmann::json details, uint32_t subaccount, const std::string& xpub)
            = 0;

        virtual void change_settings_limits(const nlohmann::json& limit_details, const nlohmann::json& twofactor_data)
            = 0;
        virtual nlohmann::json get_transactions(const nlohmann::json& details) = 0;
        virtual nlohmann::json sync_transactions(uint32_t subaccount, unique_pubkeys_and_scripts_t& missing);
        virtual void store_transactions(uint32_t subaccount, nlohmann::json& txs);
        virtual void postprocess_transactions(nlohmann::json& tx_list);
        void check_tx_memo(const std::string& memo) const;

        virtual void set_notification_handler(GA_notification_handler handler, void* context);

        virtual nlohmann::json get_receive_address(const nlohmann::json& details) = 0;
        virtual nlohmann::json get_previous_addresses(const nlohmann::json& details) = 0;
        virtual nlohmann::json get_subaccounts();
        nlohmann::json get_subaccount(uint32_t subaccount);
        virtual void update_subaccount(uint32_t subaccount, const nlohmann::json& details);

        virtual nlohmann::json get_available_currencies() const = 0;

        virtual bool is_rbf_enabled() const = 0;
        bool is_watch_only() const;
        void ensure_full_session();
        virtual nlohmann::json get_settings() const = 0;
        virtual void change_settings(const nlohmann::json& settings) = 0;

        virtual nlohmann::json get_twofactor_config(bool reset_cached = false);
        virtual std::vector<std::string> get_enabled_twofactor_methods() = 0;

        virtual void set_email(const std::string& email, const nlohmann::json& twofactor_data) = 0;
        virtual void activate_email(const std::string& code) = 0;
        virtual nlohmann::json init_enable_twofactor(
            const std::string& method, const std::string& data, const nlohmann::json& twofactor_data)
            = 0;
        virtual void enable_gauth(const std::string& code, const nlohmann::json& twofactor_data) = 0;
        virtual void enable_twofactor(const std::string& method, const std::string& code) = 0;
        virtual void disable_twofactor(const std::string& method, const nlohmann::json& twofactor_data) = 0;
        virtual nlohmann::json auth_handler_request_code(
            const std::string& method, const std::string& action, const nlohmann::json& twofactor_data)
            = 0;
        virtual std::string auth_handler_request_proxy_code(
            const std::string& action, const nlohmann::json& twofactor_data)
            = 0;
        virtual nlohmann::json request_twofactor_reset(const std::string& email) = 0;
        virtual nlohmann::json confirm_twofactor_reset(
            const std::string& email, bool is_dispute, const nlohmann::json& twofactor_data)
            = 0;

        virtual nlohmann::json request_undo_twofactor_reset(const std::string& email) = 0;
        virtual nlohmann::json confirm_undo_twofactor_reset(
            const std::string& email, const nlohmann::json& twofactor_data)
            = 0;

        virtual nlohmann::json cancel_twofactor_reset(const nlohmann::json& twofactor_data) = 0;

        virtual nlohmann::json encrypt_with_pin(const nlohmann::json& details) = 0;
        virtual nlohmann::json decrypt_with_pin(const nlohmann::json& details);

        virtual bool encache_blinding_data(const std::string& pubkey_hex, const std::string& script_hex,
            const std::string& nonce_hex, const std::string& blinding_pubkey_hex);
        virtual void encache_new_scriptpubkeys(uint32_t subaccount);
        virtual nlohmann::json get_scriptpubkey_data(byte_span_t scriptpubkey);
        virtual nlohmann::json get_address_data(const nlohmann::json& details);
        virtual void upload_confidential_addresses(
            uint32_t subaccount, const std::vector<std::string>& confidential_addresses)
            = 0;

        virtual Tx get_raw_transaction_details(const std::string& txhash_hex) const = 0;
        nlohmann::json get_transaction_details(const std::string& txhash_hex) const;

        virtual nlohmann::json service_sign_transaction(const nlohmann::json& details,
            const nlohmann::json& twofactor_data, std::vector<std::vector<unsigned char>>& old_scripts);
        virtual nlohmann::json send_transaction(const nlohmann::json& details, const nlohmann::json& twofactor_data)
            = 0;
        virtual nlohmann::json broadcast_transaction(const nlohmann::json& details) = 0;

        virtual void send_nlocktimes() = 0;
        virtual void set_csvtime(const nlohmann::json& locktime_details, const nlohmann::json& twofactor_data) = 0;
        virtual void set_nlocktime(const nlohmann::json& locktime_details, const nlohmann::json& twofactor_data) = 0;

        virtual void set_transaction_memo(const std::string& txhash_hex, const std::string& memo);

        virtual nlohmann::json get_fee_estimates() = 0;

        virtual std::string get_system_message() = 0;
        virtual std::pair<std::string, std::vector<uint32_t>> get_system_message_info(const std::string& system_message)
            = 0;
        virtual void ack_system_message(const std::string& message_hash_hex, const std::string& sig_der_hex) = 0;

        nlohmann::json cache_control(const nlohmann::json& details);

        virtual nlohmann::json convert_amount(const nlohmann::json& amount_json) const = 0;

        virtual amount get_min_fee_rate() const = 0;
        virtual amount get_default_fee_rate() const = 0;
        virtual uint32_t get_block_height() const = 0;
        amount get_dust_threshold(const std::string& asset_id) const;
        virtual nlohmann::json get_spending_limits() const;
        virtual bool is_spending_limits_decrease(const nlohmann::json& limit_details) = 0;

        virtual void save_cache();
        virtual void disable_all_pin_logins() = 0;

        const network_parameters& get_network_parameters() const { return m_net_params; }
        std::shared_ptr<signer> get_nonnull_signer(locker_t& locker);
        std::shared_ptr<signer> get_nonnull_signer();
        std::shared_ptr<signer> get_signer();
        virtual void encache_signer_xpubs(std::shared_ptr<signer> signer);
        void load_signer_xpubs(locker_t& locker, const nlohmann::json& xpubs, std::shared_ptr<signer> signer);

        virtual green_pubkeys& get_green_pubkeys();
        virtual user_pubkeys& get_user_pubkeys();
        virtual green_recovery_pubkeys& get_recovery_pubkeys();

        // Cached data
        virtual std::pair<std::string, bool> get_cached_master_blinding_key() = 0;
        void set_cached_master_blinding_key(const std::string& master_blinding_key_hex);
        virtual void set_cached_master_blinding_key_impl(locker_t& locker, const std::string& master_blinding_key_hex);

        virtual std::vector<unsigned char> output_script_from_utxo(const nlohmann::json& utxo);
        std::vector<xpub_hdkey> keys_from_utxo(const nlohmann::json& utxo);

    protected:
        // Locking per-session assumes the following thread safety model:
        // 1) Implementations noted "idempotent" can be called from multiple
        //    threads at once
        // 2) Implementations noted "post-login idempotent" can be called
        //    from multiple threads after login has completed.
        // 3) Implementations that take a locker_t as the first parameter
        //    assume that the caller holds the lock and will leave it
        //    locked upon return.
        //
        // The safest way to strictly adhere to the above is to serialize all
        // access to the session. Everything up to login should be serialized
        // otherwise. Logical wallet operations that span more than one api call
        // (such as those handled by two factor call objects) do not lock the
        // session for the entire operation. In general we must assume that
        // local state can be out of sync with the server, whether this is due
        // to multiple threads in a single process or actions in another
        // process (e.g. the user is logged in twice in different apps)

        /// Returns whether the signer was already set (i.e. true if this is a re-login)
        bool set_signer(locker_t& locker, std::shared_ptr<signer> signer);

        /// Returns true if we have a server we can write our client blob to
        bool have_client_blob_server(locker_t& locker) const;
        /// Returns true if we have a client blob we can write to
        bool have_writable_client_blob(locker_t& locker) const;

        // Sync our local blob with any server blob (No-op if no server blob)
        void sync_client_blob(locker_t& locker);

        // Load the latest blob from the server & update our local/cached copy
        bool load_client_blob(locker_t& locker, bool encache);
        virtual nlohmann::json load_client_blob_impl(locker_t& locker);

        // Save our local copy of the client blob to the server, then encache it
        bool save_client_blob(locker_t& locker, const std::string& old_hmac);
        virtual nlohmann::json save_client_blob_impl(
            locker_t& locker, const std::string& old_hmac, const std::string& blob_b64, const std::string& hmac);

        // Set our local copy of the client blob, then encache it
        // We pass the blob data as both base64 and raw bytes to account
        // for differences in derived session caches.
        void set_local_client_blob(locker_t& locker, const nlohmann::json& server_data, bool encache);

        virtual void get_cached_local_client_blob(locker_t& locker, const std::string& server_hmac) = 0;
        virtual void encache_local_client_blob(
            locker_t& locker, std::string data_b64, byte_span_t data, const std::string& hmac)
            = 0;

        // Apply an update to our local copy of the client blob. If this
        // changes the blob contents then save it to the server and encache it.
        // Repeatedly re-tries the update if the blob was altered elsewhere.
        void update_client_blob(locker_t& locker, std::function<bool()> update_fn);

        // Called when we are notified of a client blob update
        void on_client_blob_updated(nlohmann::json event);

        void subscribe_all(locker_t& locker);

        std::vector<unsigned char> output_script_from_utxo(locker_t& locker, const nlohmann::json& utxo);
        std::vector<xpub_hdkey> keys_from_utxo(locker_t& locker, const nlohmann::json& utxo);

        nlohmann::json get_proxy_settings(locker_t& locker);
        nlohmann::json get_net_call_params(locker_t& locker, uint32_t timeout_secs);

        bool is_twofactor_reset_active(locker_t& locker) const;

        virtual nlohmann::json get_subaccounts_impl(locker_t& locker) = 0;

        // ** Under no circumstances must this mutex ever be made recursive **
        mutable std::mutex m_mutex;

        // Immutable upon construction
        const network_parameters m_net_params;
        io_runner<1> m_io;
        std::unique_ptr<boost::asio::io_context::strand> m_strand;

        const std::string m_user_proxy;
        std::shared_ptr<tor_controller> m_tor_ctrl;

        // Immutable once set by the caller (prior to connect)
        GA_notification_handler m_notification_handler;
        void* m_notification_context;

        // Immutable post-login
        nlohmann::json m_login_data;
        std::shared_ptr<signer> m_signer;
        std::unique_ptr<green_pubkeys> m_green_pubkeys;
        std::unique_ptr<user_pubkeys> m_user_pubkeys;
        std::unique_ptr<green_recovery_pubkeys> m_recovery_pubkeys;
        bool m_watch_only;

        // Mutable
        std::string m_tor_proxy; // Updated on connect(), protected by m_mutex
        std::atomic_bool m_notify; // Whether to emit notifications

        // Current client blob
        std::unique_ptr<client_blob> m_blob;

        // UTXOs
        // Cached UTXOs are unfiltered; if using the cached values you
        // may need to filter them first (e.g. to removed expired or frozen UTXOS)
        using utxo_cache_key_t = std::pair<uint32_t, uint32_t>; // subaccount, num_confs
        using utxo_cache_t = std::map<utxo_cache_key_t, utxo_cache_value_t>;
        mutable std::mutex m_utxo_cache_mutex;
        utxo_cache_t m_utxo_cache;

        std::vector<std::shared_ptr<wamp_transport>> m_wamp_connections;
        std::shared_ptr<wamp_transport> m_blobserver;
    };

} // namespace green

#endif // #ifndef GDK_SESSION_IMPL_HPP
