#include <iostream>
#include <boost/asio.hpp>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>

using boost::asio::ip::tcp;

class session : public std::enable_shared_from_this<session>{
    public:
      session(tcp::socket socket, FileManager& file_manager)
          : socket_(std::move(socket)), file_manager_(file_manager) {}
    
      void start() {
          read_command();
      }
  private:
      void read_command() {
          auto self(shared_from_this());
          boost::asio::async_read_until(socket_, buffer_, "\r\n",
              [this, self](boost::system::error_code ec, std::size_t length) {
                  if (!ec) {
                      std::istream is(&buffer_);
                      std::string command;
                      std::getline(is, command);

                      // Remove \r se presente
                      if (!command.empty() && command.back() == '\r') {
                          command.pop_back();
                      }

                      process_command(command);
                      read_command();
                  }
              });
      }

      void process_command(const std::string& command) {
          std::vector<std::string> tokens;
          boost::split(tokens, command, boost::is_any_of("|"));

          if (tokens.empty()) return;

          if (tokens[0] == "LOG" && tokens.size() == 4) {
              handle_log(tokens[1], tokens[2], tokens[3]);
          } else if (tokens[0] == "GET" && tokens.size() == 3) {
              handle_get(tokens[1], tokens[2]);
          }
      }

      void handle_log(const std::string& sensor_id, 
                     const std::string& timestamp, 
                     const std::string& value_str) {
          try {
              LogRecord record;
              std::strncpy(record.sensor_id, sensor_id.c_str(), sizeof(record.sensor_id) - 1);
              record.sensor_id[sizeof(record.sensor_id) - 1] = '\0';
              record.timestamp = string_to_time_t(timestamp);
              record.value = std::stod(value_str);

              file_manager_.log_record(record);
          } catch (const std::exception& e) {
              std::cerr << "Erro no LOG: " << e.what() << std::endl;
          }
      }

      void handle_get(const std::string& sensor_id, const std::string& count_str) {
          try {
              int count = std::stoi(count_str);
              auto records = file_manager_.get_last_n_records(sensor_id, count);

              if (records.empty()) {
                  // Verifica se o arquivo existe para determinar se é ID inválido
                  std::string filename = "logs/" + sensor_id + ".bin";
                  if (!fs::exists(filename)) {
                      throw std::runtime_error("Sensor ID não existe");
                  }
              }

              std::ostringstream response;
              response << records.size() << ";";
              for (const auto& record : records) {
                  response << time_t_to_string(record.timestamp) 
                           << "|" << record.value << ";";
              }

              // Remove último ponto-e-vírgula
              std::string response_str = response.str();
              if (!response_str.empty() && response_str.back() == ';') {
                  response_str.pop_back();
              }
              response_str += "\r\n";

              boost::asio::write(socket_, boost::asio::buffer(response_str));
          } catch (const std::exception& e) {
              std::cerr << "Erro no GET: " << e.what() << std::endl;
              std::string error = "ERROR|INVALID_SENSOR_ID\r\n";
              boost::asio::write(socket_, boost::asio::buffer(error));
          }
      }

      tcp::socket socket_;
      boost::asio::streambuf buffer_;
      FileManager& file_manager_;
  };


class AsyncServer {
public:
    AsyncServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          file_manager_() {
        // Criar diretório de logs se não existir
        if (!fs::exists("logs")) {
            fs::create_directory("logs");
        }
        
        accept_connection();
    }

private:
    void accept_connection() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), file_manager_)->start();
                } else {
                    std::cerr << "Erro de aceitação: " << ec.message() << std::endl;
                }
                accept_connection();
            });
    }

    tcp::acceptor acceptor_;
    FileManager file_manager_;
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

std::vector<char> serialize_log_record(const LogRecord& record) {
    std::vector<char> buffer(sizeof(LogRecord));
    std::memcpy(buffer.data(), &record, sizeof(LogRecord));
    return buffer;
}

LogRecord deserialize_log_record(const char* data) {
    LogRecord record;
    std::memcpy(&record, data, sizeof(LogRecord));
    return record;
}

class FileManager {
public:
    void log_record(const LogRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string filename = "logs/" + std::string(record.sensor_id) + ".bin";
        std::ofstream file(filename, std::ios::binary | std::ios::app);
        if (!file) {
            std::cerr << "Erro ao abrir arquivo: " << filename << std::endl;
            return;
        }
        auto data = serialize_log_record(record);
        file.write(data.data(), data.size());
    }

    std::vector<LogRecord> get_last_n_records(const std::string& sensor_id, int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string filename = "logs/" + sensor_id + ".bin";
        std::ifstream file(filename, std::ios::binary);
        std::vector<LogRecord> records;
        
        if (!file) {
            return records;
        }

        file.seekg(0, std::ios::end);
        auto size = file.tellg();
        auto count = size / sizeof(LogRecord);
        auto to_read = std::min(static_cast<int>(count), n);
        
        if (to_read > 0) {
            file.seekg(-to_read * sizeof(LogRecord), std::ios::end);
            records.resize(to_read);
            file.read(reinterpret_cast<char*>(records.data()), to_read * sizeof(LogRecord));
        }
        
        return records;
    }

private:
    std::mutex mutex_;
};

int main(int argc, char* argv[]) {
    try {
        boost::asio::io_context io_context;
        AsyncServer server(io_context, 9000);
        
        std::cout << "Servidor de aquisição de dados iniciado na porta 9000" << std::endl;
        std::cout << "Diretório de logs: " << fs::absolute("logs") << std::endl;
        
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Erro fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}