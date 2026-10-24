#include <cppgate/TrustedProxies.h>

#include <algorithm>
#include <stdexcept>

namespace gtvr {

    namespace {

        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        boost::asio::ip::address normalize(const boost::asio::ip::address& address)
        {
            if (address.is_v6() && address.to_v6().is_v4_mapped())
                return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());
            return address;
        }

    }

    std::optional<boost::asio::ip::address> TrustedProxies::parseAddress(std::string_view text)
    {
        text = trim(text);

        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);

        if (!text.empty() && text.front() == '[')
        {
            // [ipv6] or [ipv6]:port
            auto end = text.find(']');
            if (end == std::string_view::npos)
                return std::nullopt;
            text = text.substr(1, end - 1);
        }
        else if (std::count(text.begin(), text.end(), ':') == 1)
        {
            // ipv4:port, a bare ipv6 has at least two colons
            text = text.substr(0, text.find(':'));
        }

        boost::system::error_code ec;
        auto address = boost::asio::ip::make_address(std::string(text), ec);
        if (ec)
            return std::nullopt;

        return normalize(address);
    }

    void TrustedProxies::add(std::string_view cidr)
    {
        auto slash = cidr.find('/');
        auto address = parseAddress(cidr.substr(0, slash));
        if (!address)
            throw std::invalid_argument("TrustedProxies: invalid address: " + std::string(cidr));

        unsigned long prefix = address->is_v4() ? 32 : 128;
        if (slash != std::string_view::npos)
        {
            auto digits = std::string(cidr.substr(slash + 1));
            if (digits.empty() || digits.size() > 3 || digits.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("TrustedProxies: invalid prefix: " + std::string(cidr));
            prefix = std::stoul(digits);
        }

        try {
            if (address->is_v4())
                v4_.push_back(boost::asio::ip::network_v4(address->to_v4(), static_cast<unsigned short>(prefix)).canonical());
            else
                v6_.push_back(boost::asio::ip::network_v6(address->to_v6(), static_cast<unsigned short>(prefix)).canonical());
        }
        catch (const std::out_of_range&) {
            throw std::invalid_argument("TrustedProxies: invalid prefix: " + std::string(cidr));
        }
    }

    bool TrustedProxies::contains(const boost::asio::ip::address& address) const
    {
        auto ip = normalize(address);

        if (ip.is_v4())
        {
            for (const auto& network : v4_)
                if (boost::asio::ip::network_v4(ip.to_v4(), network.prefix_length()).canonical() == network)
                    return true;
        }
        else
        {
            for (const auto& network : v6_)
                if (boost::asio::ip::network_v6(ip.to_v6(), network.prefix_length()).canonical() == network)
                    return true;
        }

        return false;
    }

    std::string TrustedProxies::resolve(std::string_view peer, const boost::beast::http::fields& headers) const
    {
        auto peer_address = parseAddress(peer);
        if (!peer_address)
            return std::string(peer);

        // Headers from anyone else than our proxies are not trusted
        if (empty() || !contains(*peer_address))
            return peer_address->to_string();

        // Several X-Forwarded-For headers are one list, in order
        std::vector<std::string_view> hops;
        auto [first, last] = headers.equal_range("X-Forwarded-For");
        for (auto it = first; it != last; ++it)
        {
            std::string_view value = it->value();
            size_t start = 0;
            while (start <= value.size())
            {
                auto comma = value.find(',', start);
                if (comma == std::string_view::npos)
                    comma = value.size();
                hops.push_back(value.substr(start, comma - start));
                start = comma + 1;
            }
        }

        if (!hops.empty())
        {
            // Right to left: the closest hop first, the first one we don't trust is the client
            auto closest_trusted = *peer_address;
            for (auto hop = hops.rbegin(); hop != hops.rend(); ++hop)
            {
                auto address = parseAddress(*hop);

                // Garbage in the chain: nothing to the left of it can be trusted
                if (!address)
                    return closest_trusted.to_string();

                if (!contains(*address))
                    return address->to_string();

                closest_trusted = *address;
            }

            // Every hop is one of our proxies
            return closest_trusted.to_string();
        }

        auto real_ip = headers.find("X-Real-IP");
        if (real_ip != headers.end())
        {
            if (auto address = parseAddress(real_ip->value()))
                return address->to_string();
        }

        return peer_address->to_string();
    }

}//namespace gtvr
