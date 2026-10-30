#pragma once

#include <string>
#include <vector>

namespace gtvr {

    // HTTPS / WSS trust settings, shared by HttpClient::Options and WebSocketClient::Options
    struct ClientTlsOptions
    {
        // Verify the server's certificate and host name. Only turn off for local development.
        bool                        verify_peer                 = true;

        // Where trusted CAs come from; any combination. With verify_peer and none of them,
        // the client's constructor throws.

        // The Mozilla CA store bundled with cppgate (see tools/update_ca_bundle.py)
        bool                        use_bundled_cas             = true;

        // The operating system's store: $SSL_CERT_FILE / $SSL_CERT_DIR if set, and the first
        // well-known bundle found (/etc/ssl/certs/ca-certificates.crt on Debian/Ubuntu,
        // /etc/pki/tls/certs/ca-bundle.crt on Fedora/RHEL, /etc/ssl/cert.pem on macOS...).
        // Windows has no such file: there only the environment variables count.
        bool                        use_system_cas              = false;

        // More trusted CAs: PEM files (one or many certificates each), directories in OpenSSL's
        // hashed format (<subject hash>.0, as made by c_rehash / openssl rehash), or PEM text
        std::vector<std::string>    extra_ca_files;
        std::vector<std::string>    extra_ca_dirs;
        std::vector<std::string>    extra_ca_pem;
    };

}//namespace gtvr
