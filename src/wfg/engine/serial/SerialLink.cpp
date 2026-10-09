/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <wfg/engine/serial/SerialLink.h>

#include <algorithm>
#include <array>
#include <filesystem>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <cerrno>
 #include <cstring>
 #include <fcntl.h>
 #include <poll.h>
 #include <termios.h>
 #include <unistd.h>
#endif

namespace wfg::serial
{
    bool isStandardBaud (int baud) noexcept
    {
        static constexpr std::array<int, 11> speeds { 1200, 2400, 4800, 9600, 19200, 38400, 57600,
                                                      115200, 230400, 460800, 921600 };
        return std::find (speeds.begin(), speeds.end(), baud) != speeds.end();
    }

#if defined (_WIN32)
    namespace
    {
        std::string lastError (const char* what)
        {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND)
                return std::string (what) + ": no such port on this machine";
            if (code == ERROR_ACCESS_DENIED)
                return std::string (what) + ": the port is in use by another program";
            return std::string (what) + ": Windows error " + std::to_string (code);
        }

        std::wstring widened (const std::string& text)
        {
            if (text.empty())
                return {};
            const auto size = MultiByteToWideChar (CP_UTF8, 0, text.data(), static_cast<int> (text.size()), nullptr, 0);
            std::wstring out (static_cast<std::size_t> (size), L'\0');
            MultiByteToWideChar (CP_UTF8, 0, text.data(), static_cast<int> (text.size()), out.data(), size);
            return out;
        }

        std::string narrowed (const std::wstring& text)
        {
            if (text.empty())
                return {};
            const auto size = WideCharToMultiByte (CP_UTF8, 0, text.data(), static_cast<int> (text.size()),
                                                   nullptr, 0, nullptr, nullptr);
            std::string out (static_cast<std::size_t> (size), '\0');
            WideCharToMultiByte (CP_UTF8, 0, text.data(), static_cast<int> (text.size()), out.data(), size,
                                 nullptr, nullptr);
            return out;
        }

        class WindowsLink final : public Link
        {
        public:
            explicit WindowsLink (HANDLE h) : handle (h) {}
            ~WindowsLink() override { CloseHandle (handle); }

            std::optional<std::string> read (std::chrono::milliseconds wait) override
            {
                /*  Back at once with what is there; otherwise waiting for the
                    first byte at most `wait` - Windows' own reading of these
                    three numbers together. */
                COMMTIMEOUTS timeouts {};
                timeouts.ReadIntervalTimeout = MAXDWORD;
                timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
                timeouts.ReadTotalTimeoutConstant = static_cast<DWORD> (std::max<long long> (1, wait.count()));
                timeouts.WriteTotalTimeoutConstant = 200;
                if (! SetCommTimeouts (handle, &timeouts))
                {
                    failure = lastError ("reading");
                    return std::nullopt;
                }

                std::array<char, 1024> buffer {};
                DWORD got = 0;
                if (! ReadFile (handle, buffer.data(), static_cast<DWORD> (buffer.size()), &got, nullptr))
                {
                    failure = lastError ("reading");
                    return std::nullopt;
                }
                return std::string (buffer.data(), got);
            }

            bool write (const std::string& bytes) override
            {
                DWORD wrote = 0;
                if (! WriteFile (handle, bytes.data(), static_cast<DWORD> (bytes.size()), &wrote, nullptr))
                {
                    failure = lastError ("writing");
                    return false;
                }
                return true;
            }

            std::string problem() const override { return failure; }

        private:
            HANDLE handle;
            std::string failure;
        };
    }

    std::unique_ptr<Link> openSystemPort (const std::string& path, int baud, std::string& problem)
    {
        if (! isStandardBaud (baud))
        {
            problem = "a speed of " + std::to_string (baud) + " is not one a port opens at";
            return nullptr;
        }

        //  COM10 and above are reached only as \\.\COM10; the prefix does no harm below it.
        auto name = path;
        if (name.rfind ("\\\\.\\", 0) != 0)
            name = "\\\\.\\" + name;

        const auto handle = CreateFileW (widened (name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            problem = lastError (path.c_str());
            return nullptr;
        }

        DCB dcb {};
        dcb.DCBlength = sizeof (dcb);
        if (! GetCommState (handle, &dcb))
        {
            problem = lastError (path.c_str());
            CloseHandle (handle);
            return nullptr;
        }
        dcb.BaudRate = static_cast<DWORD> (baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        if (! SetCommState (handle, &dcb))
        {
            problem = lastError (path.c_str());
            CloseHandle (handle);
            return nullptr;
        }

        PurgeComm (handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return std::make_unique<WindowsLink> (handle);
    }

    std::vector<SystemPort> systemPorts()
    {
        std::vector<SystemPort> out;
        HKEY key = nullptr;
        if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
            return out;

        for (DWORD index = 0;; ++index)
        {
            std::array<wchar_t, 256> name {};
            std::array<wchar_t, 256> data {};
            DWORD nameSize = static_cast<DWORD> (name.size());
            DWORD dataSize = static_cast<DWORD> (data.size() * sizeof (wchar_t));
            DWORD type = 0;
            const auto result = RegEnumValueW (key, index, name.data(), &nameSize, nullptr, &type,
                                               reinterpret_cast<LPBYTE> (data.data()), &dataSize);
            if (result == ERROR_NO_MORE_ITEMS)
                break;
            if (result != ERROR_SUCCESS || type != REG_SZ)
                continue;

            //  The value's name is the driver's device - \Device\USBSER000 for an Arduino's.
            auto about = narrowed (std::wstring (name.data(), nameSize));
            if (const auto slash = about.find_last_of ('\\'); slash != std::string::npos)
                about = about.substr (slash + 1);
            out.push_back ({ narrowed (std::wstring (data.data())), about });
        }
        RegCloseKey (key);

        std::sort (out.begin(), out.end(), [] (const SystemPort& a, const SystemPort& b)
        {
            return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
        });
        return out;
    }

#else
    namespace
    {
        std::string lastError (const std::string& what)
        {
            const auto code = errno;
            if (code == ENOENT)
                return what + ": no such port on this machine";
            if (code == EBUSY)
                return what + ": the port is in use by another program";
            if (code == EACCES)
                return what + ": not allowed to open it (on Linux, the dialout group)";
            return what + ": " + std::strerror (code);
        }

        std::optional<speed_t> speedOf (int baud)
        {
            switch (baud)
            {
                case 1200:   return B1200;
                case 2400:   return B2400;
                case 4800:   return B4800;
                case 9600:   return B9600;
                case 19200:  return B19200;
                case 38400:  return B38400;
                case 57600:  return B57600;
                case 115200: return B115200;
                case 230400: return B230400;
               #if defined (B460800)
                case 460800: return B460800;
               #endif
               #if defined (B921600)
                case 921600: return B921600;
               #endif
                default:     return std::nullopt;
            }
        }

        class PosixLink final : public Link
        {
        public:
            explicit PosixLink (int descriptor) : fd (descriptor) {}
            ~PosixLink() override { ::close (fd); }

            std::optional<std::string> read (std::chrono::milliseconds wait) override
            {
                pollfd watched { fd, POLLIN, 0 };
                const auto ready = ::poll (&watched, 1, static_cast<int> (wait.count()));
                if (ready < 0)
                {
                    if (errno == EINTR)
                        return std::string {};
                    failure = lastError ("reading");
                    return std::nullopt;
                }
                if (ready == 0)
                    return std::string {};
                if ((watched.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (watched.revents & POLLIN) == 0)
                {
                    failure = "the port went away";
                    return std::nullopt;
                }

                std::array<char, 1024> buffer {};
                const auto got = ::read (fd, buffer.data(), buffer.size());
                if (got < 0)
                {
                    if (errno == EAGAIN || errno == EINTR)
                        return std::string {};
                    failure = lastError ("reading");
                    return std::nullopt;
                }
                if (got == 0)
                {
                    failure = "the port went away";
                    return std::nullopt;
                }
                return std::string (buffer.data(), static_cast<std::size_t> (got));
            }

            bool write (const std::string& bytes) override
            {
                std::size_t done = 0;
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (200);
                while (done < bytes.size())
                {
                    const auto wrote = ::write (fd, bytes.data() + done, bytes.size() - done);
                    if (wrote > 0)
                    {
                        done += static_cast<std::size_t> (wrote);
                        continue;
                    }
                    if (wrote < 0 && errno != EAGAIN && errno != EINTR)
                    {
                        failure = lastError ("writing");
                        return false;
                    }
                    if (std::chrono::steady_clock::now() > until)
                        return true;     // the rest is dropped rather than the port held
                    pollfd watched { fd, POLLOUT, 0 };
                    ::poll (&watched, 1, 20);
                }
                return true;
            }

            std::string problem() const override { return failure; }

        private:
            int fd;
            std::string failure;
        };
    }

    std::unique_ptr<Link> openSystemPort (const std::string& path, int baud, std::string& problem)
    {
        const auto speed = speedOf (baud);
        if (! speed.has_value())
        {
            problem = "a speed of " + std::to_string (baud) + " is not one this system opens a port at";
            return nullptr;
        }

        const auto fd = ::open (path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            problem = lastError (path);
            return nullptr;
        }

        termios settings {};
        if (::tcgetattr (fd, &settings) != 0)
        {
            problem = lastError (path);
            ::close (fd);
            return nullptr;
        }
        ::cfmakeraw (&settings);
        settings.c_cflag |= static_cast<tcflag_t> (CLOCAL | CREAD);
        settings.c_cflag &= static_cast<tcflag_t> (~(PARENB | CSTOPB | CSIZE));
        settings.c_cflag |= static_cast<tcflag_t> (CS8);
       #if defined (CRTSCTS)
        settings.c_cflag &= static_cast<tcflag_t> (~CRTSCTS);
       #endif
        settings.c_iflag &= static_cast<tcflag_t> (~(IXON | IXOFF | IXANY));
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        ::cfsetispeed (&settings, *speed);
        ::cfsetospeed (&settings, *speed);
        if (::tcsetattr (fd, TCSANOW, &settings) != 0)
        {
            problem = lastError (path);
            ::close (fd);
            return nullptr;
        }
        ::tcflush (fd, TCIOFLUSH);
        return std::make_unique<PosixLink> (fd);
    }

    std::vector<SystemPort> systemPorts()
    {
        namespace fs = std::filesystem;
        std::vector<SystemPort> out;
        std::error_code ignored;

       #if defined (__APPLE__)
        for (const auto& entry : fs::directory_iterator ("/dev", ignored))
        {
            const auto name = entry.path().filename().string();
            if (name.rfind ("cu.", 0) == 0 && name != "cu.Bluetooth-Incoming-Port")
                out.push_back ({ entry.path().string(), name.substr (3) });
        }
       #else
        //  The stable names first: the same Arduino keeps its by-id name when it moves port.
        std::vector<std::string> named;
        for (const auto& entry : fs::directory_iterator ("/dev/serial/by-id", ignored))
        {
            const auto target = fs::canonical (entry.path(), ignored);
            out.push_back ({ entry.path().string(), target.empty() ? std::string {} : target.filename().string() });
            if (! target.empty())
                named.push_back (target.string());
        }
        for (const auto& entry : fs::directory_iterator ("/dev", ignored))
        {
            const auto name = entry.path().filename().string();
            if ((name.rfind ("ttyACM", 0) == 0 || name.rfind ("ttyUSB", 0) == 0)
                  && std::find (named.begin(), named.end(), entry.path().string()) == named.end())
                out.push_back ({ entry.path().string(), name });
        }
       #endif

        std::sort (out.begin(), out.end(), [] (const SystemPort& a, const SystemPort& b) { return a.path < b.path; });
        return out;
    }
#endif
}
