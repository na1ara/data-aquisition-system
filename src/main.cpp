#include <iostream>
#include <boost/asio.hpp>
#include <ctime>
#include <iomanip>
#include <sstream>

using boost::asio::ip::tcp;

class session : public std::enable_shared_from_this<session>{
    public:
        session(tcp::socket socket) : socket_(std::move(socket)){}

        void start(){
            read_message;
        }
    private:
        void read_message(){
            auto self(shared_from_this());
            boost::asio::async_read_until(socket_, buffer_, "\r\n",
            [this, self](boost::system::error_code ec, std::size_t length)
            {
            if (!ec)
            {
                std::istream is(&buffer_);
                std::string message(std::istreambuf_iterator<char>(is), {});
                std::cout << "Received: " << message << std::endl;
                write_message(message);
            }
            });
        }

        void write_message(const std::string& message){
            auto self(shared_from_this());
            boost::asio::async_write(socket_, boost::asio::buffer(message),
                [this, self, message](boost::system::error_code ec, std::size_t /*length*/)
                {
                  if (!ec){
                    read_message();
                  }
                });
        }

        tcp::socket socket_;
        boost::asio::streambuf buffer_;
}


class server{
    public:
      server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)){
        accept();
      }

    private:
      void accept(){
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
            {
              if (!ec){
                std::make_shared<session>(std::move(socket))->start();
              }

              accept();
            });
      }

      tcp::acceptor acceptor_;
};

#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32]; // supondo um ID de sensor de até 32 caracteres
    std::time_t timestamp; // timestamp UNIX
    double value; // valor da leitura
};
#pragma pack(pop)

std::time_t string_to_time_t(const std::string& time_string) {
    std::tm tm = {};
    std::istringstream ss(time_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

std::string time_t_to_string(std::time_t time) {
    std::tm* tm = std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

std::vector<char> new_message(const LogRecord& record) {
    std::vector<char> buffer;
    buffer.resize(sizeof(LogRecord));

    std::memcpy(buffer.data(), &record, sizeof(LogRecord));
    
    return buffer;
}

int main(int argc, char* argv[]) {
    LogRecord data;

    if (argc != 2){
        std::cerr << "Usage: chat_server <port>\n";
        return 1;
    }

    boost::asio::io_context io_context;

    server s(io_context, std::atoi(argv[1]));

    io_context.run();
    return 0;
}