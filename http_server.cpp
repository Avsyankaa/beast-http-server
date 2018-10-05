#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using tcp = boost::asio::ip::tcp;     // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;  // from <boost/beast/http.hpp>

struct FrindlyUserServer
{
public:
    FrindlyUserServer(boost::asio::ip::address address, unsigned short port)
        : ioc {1}, acceptor {ioc, {address, port}}, signals(ioc) {
        signals.async_wait(
            [this](boost::system::error_code const&, int) { this->ioc.stop(); });
    }

    void AddStopSignal(int signal) { signals.add(signal); }

    void Start()
    {
        for (;;) {
            tcp::socket socket {ioc};

            acceptor.accept(socket);

            std::thread {
                std::bind(&FrindlyUserServer::DoSession, this, std::move(socket))}
            .detach();
        }
    }

protected:
    virtual void DoSession(tcp::socket& socket) = 0;

private:
    boost::asio::io_context ioc;
    tcp::acceptor acceptor;
    boost::asio::signal_set signals;
};

struct SimpleServer : public FrindlyUserServer
{
    SimpleServer(boost::asio::ip::address address, unsigned short port)
        : FrindlyUserServer(address, port)
    { }

protected:
    void DoSession(tcp::socket& socket) override
    {
        bool close = false;
        boost::system::error_code ec;

        // This buffer is required to persist across reads
        boost::beast::flat_buffer buffer;

        // This lambda is used to send messages
        send_lambda<tcp::socket> send {socket, close, ec};

        for (;;) {
            // Read a request
            http::request<http::string_body> req;
            http::read(socket, buffer, req, ec);
            if (ec == http::error::end_of_stream)
                break;
            if (ec)
                return fail(ec, "read");

            // Send the response
            send(HandleRequest(std::move(req)));

            if (ec)
                return fail(ec, "write");
            if (close) {
                // This means we should close the connection, usually because
                // the response indicated the "Connection: close" semantic.
                break;
            }
        }

        // Send a TCP shutdown
        socket.shutdown(tcp::socket::shutdown_send, ec);
    }

    virtual http::response<http::string_body> HandleRequest(
        http::request<http::string_body>&& req) = 0;

private:
    // This is the C++11 equivalent of a generic lambda.
    // The function object is used to send an HTTP message.
    template <class Stream>
    struct send_lambda
    {
        Stream& stream_;
        bool& close_;
        boost::system::error_code& ec_;

        explicit send_lambda(Stream& stream,
                             bool& close,
                             boost::system::error_code& ec)
            : stream_(stream), close_(close), ec_(ec)
        { }

        template <bool isRequest, class Body, class Fields>
        void operator()(http::message<isRequest, Body, Fields>&& msg) const
        {
            // Determine if we should close the connection after
            close_ = msg.need_eof();

            // We need the serializer here because the serializer requires
            // a non-const file_body, and the message oriented version of
            // http::write only works with const messages.
            http::serializer<isRequest, Body, Fields> sr {msg};
            http::write(stream_, sr, ec_);
        }
    };

    void fail(boost::system::error_code ec, char const* what)
    {
        std::cerr << what << ": " << ec.message() << "\n";
    }
};

struct HelloWorld : public SimpleServer
{
    HelloWorld(boost::asio::ip::address address, unsigned short port)
        : SimpleServer(address, port)
    { }

protected:
    http::response<http::string_body> HandleRequest(
        http::request<http::string_body>&& req) override
    {
        http::response<http::string_body> res {http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "application/json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"Hello, World"})";
        res.prepare_payload();
        return res;
    }
};

//------------------------------------------------------------------------------
std::string getOSEnv(boost::string_view name, boost::string_view default_value)
{
    const char* e = std::getenv(name.data());
    return e ? e : default_value.data();
}

int main(int /*argc*/, char** /*argv*/)
{
    try {
        auto const address = boost::asio::ip::make_address("0.0.0.0");
        auto const port_arg = getOSEnv("PORT", "5000");
        auto const port = static_cast<unsigned short>(std::atoi(port_arg.c_str()));
        auto const doc_root = std::make_shared<std::string>("Hello world");

        HelloWorld server(address, port);
        server.AddStopSignal(SIGINT);
        server.AddStopSignal(SIGTERM);
        server.Start();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
