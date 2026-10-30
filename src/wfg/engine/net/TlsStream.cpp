// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/net/TlsStream.h>

#include <juce_core/juce_core.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <mutex>

namespace wfg::net
{
    namespace
    {
        /*  PSA CRYPTO ONCE: Mbed TLS 3.6's TLS 1.3 asks the PSA layer for its
            keys, and the layer must be started before the first handshake. */
        bool psaReady()
        {
            static std::once_flag once;
            static bool ready = false;
            std::call_once (once, [] { ready = psa_crypto_init() == PSA_SUCCESS; });
            return ready;
        }

        std::string errorText (int code)
        {
            std::array<char, 160> text {};
            mbedtls_strerror (code, text.data(), text.size());
            return text.data();
        }
    }

    std::string normalisedFingerprint (const std::string& text)
    {
        std::string out;

        for (const char c : text)
            if (std::isxdigit (static_cast<unsigned char> (c)) != 0)
                out.push_back (static_cast<char> (std::toupper (static_cast<unsigned char> (c))));

        return out;
    }

    struct TlsStream::Impl
    {
        juce::StreamingSocket socket;
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config config;
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context random;
        bool ready = false;
        int waitMs = 0;
        std::string fingerprint;
        std::string problem;

        Impl()
        {
            mbedtls_ssl_init (&ssl);
            mbedtls_ssl_config_init (&config);
            mbedtls_entropy_init (&entropy);
            mbedtls_ctr_drbg_init (&random);
        }

        ~Impl()
        {
            if (ready)
                mbedtls_ssl_close_notify (&ssl);

            mbedtls_ssl_free (&ssl);
            mbedtls_ssl_config_free (&config);
            mbedtls_ctr_drbg_free (&random);
            mbedtls_entropy_free (&entropy);
        }

        /*  THE BYTES GO THROUGH JUCE'S SOCKET: written whole, read with the
            wait the caller asked for - a read that finds nothing in it is
            Mbed TLS's timeout, one that finds the stream closed its end. */
        static int send (void* context, const unsigned char* bytes, std::size_t count)
        {
            auto& self = *static_cast<Impl*> (context);
            const auto written = self.socket.write (bytes, static_cast<int> (count));
            return written < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : written;
        }

        static int receive (void* context, unsigned char* bytes, std::size_t count, std::uint32_t)
        {
            auto& self = *static_cast<Impl*> (context);
            const auto ready = self.socket.waitUntilReady (true, self.waitMs);

            if (ready < 0)
                return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

            if (ready == 0)
                return MBEDTLS_ERR_SSL_TIMEOUT;

            const auto got = self.socket.read (bytes, static_cast<int> (count), false);
            return got < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : got;
        }
    };

    TlsStream::TlsStream() : impl (std::make_unique<Impl>()) {}
    TlsStream::~TlsStream() = default;

    const std::string& TlsStream::fingerprint() const noexcept { return impl->fingerprint; }
    const std::string& TlsStream::problem() const noexcept { return impl->problem; }

    bool TlsStream::open (const std::string& host, int port, int timeoutMs, std::string& problem)
    {
        auto& self = *impl;

        if (! psaReady())
        {
            problem = "the TLS library would not start";
            return false;
        }

        if (! self.socket.connect (juce::String (host), port, timeoutMs))
        {
            problem = host + ":" + std::to_string (port) + ": nothing answered";
            return false;
        }

        static constexpr char personal[] = "wfg-tls";

        if (const auto code = mbedtls_ctr_drbg_seed (&self.random, mbedtls_entropy_func, &self.entropy,
                                                     reinterpret_cast<const unsigned char*> (personal), sizeof (personal) - 1);
            code != 0)
        {
            problem = "no random numbers for TLS: " + errorText (code);
            return false;
        }

        mbedtls_ssl_config_defaults (&self.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode (&self.config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng (&self.config, mbedtls_ctr_drbg_random, &self.random);

        if (const auto code = mbedtls_ssl_setup (&self.ssl, &self.config); code != 0)
        {
            problem = "TLS would not set up: " + errorText (code);
            return false;
        }

        //  The name, for a server that picks its certificate by it; never a number.
        if (juce::IPAddress (juce::String (host)).isNull())
            mbedtls_ssl_set_hostname (&self.ssl, host.c_str());

        mbedtls_ssl_set_bio (&self.ssl, &self, Impl::send, nullptr, Impl::receive);
        self.waitMs = timeoutMs;

        for (;;)
        {
            const auto code = mbedtls_ssl_handshake (&self.ssl);

            if (code == 0)
                break;

            if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;

            problem = code == MBEDTLS_ERR_SSL_TIMEOUT ? std::string ("the TLS handshake took too long")
                                                      : "the TLS handshake failed: " + errorText (code);
            return false;
        }

        self.ready = true;

        if (const auto* certificate = mbedtls_ssl_get_peer_cert (&self.ssl); certificate != nullptr)
        {
            std::array<unsigned char, 32> digest {};
            mbedtls_sha256 (certificate->raw.p, certificate->raw.len, digest.data(), 0);

            for (std::size_t at = 0; at < digest.size(); ++at)
            {
                std::array<char, 4> pair {};
                std::snprintf (pair.data(), pair.size(), "%02X", static_cast<unsigned> (digest[at]));
                self.fingerprint += (at > 0 ? ":" : "") + std::string (pair.data());
            }
        }

        if (self.fingerprint.empty())
        {
            problem = "the far end presented no certificate";
            return false;
        }

        return true;
    }

    int TlsStream::read (std::string& into, int waitMs)
    {
        auto& self = *impl;

        if (! self.ready)
            return -1;

        self.waitMs = waitMs;
        std::array<unsigned char, 16384> buffer {};

        for (;;)
        {
            const auto got = mbedtls_ssl_read (&self.ssl, buffer.data(), buffer.size());

            if (got > 0)
            {
                into.append (reinterpret_cast<const char*> (buffer.data()), static_cast<std::size_t> (got));
                return got;
            }

            //  A TLS 1.3 server's session ticket is no data and no end.
            if (got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;

            if (got == MBEDTLS_ERR_SSL_TIMEOUT || got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE)
                return 0;

            self.problem = got == 0 || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? std::string ("the far end closed the connection")
                                                                               : "the connection failed: " + errorText (got);
            self.ready = false;
            return -1;
        }
    }

    bool TlsStream::write (std::string_view bytes)
    {
        auto& self = *impl;
        std::size_t done = 0;

        while (self.ready && done < bytes.size())
        {
            const auto wrote = mbedtls_ssl_write (&self.ssl, reinterpret_cast<const unsigned char*> (bytes.data() + done),
                                                  bytes.size() - done);

            if (wrote > 0)
            {
                done += static_cast<std::size_t> (wrote);
                continue;
            }

            if (wrote == MBEDTLS_ERR_SSL_WANT_READ || wrote == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;

            self.problem = "the connection failed while writing: " + errorText (wrote);
            self.ready = false;
        }

        return self.ready && done == bytes.size();
    }
}
