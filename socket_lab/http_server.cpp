#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

std::queue<int> task_queue;
std::mutex queue_mutex;
std::condition_variable cv;
bool stop_pool = false;
const int NUM_THREADS = 10;

std::string get_content_type(const std::string& path) {
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".txt")) return "text/plain";
    return "application/octet-stream";
}

std::string parse_request(int client_fd) {
    char buffer[4096];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) return "";

    buffer[bytes_read] = '\0';
    std::string request(buffer);

    size_t pos = request.find('\n');
    if (pos == std::string::npos) return "";

    std::string first_line = request.substr(0, pos);
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }

    size_t method_end = first_line.find(' ');
    if (method_end == std::string::npos) return "400 Bad Request";

    std::string method = first_line.substr(0, method_end);
    if (method != "GET") return "400 Bad Request";

    size_t path_end = first_line.find(' ', method_end + 1);
    if (path_end == std::string::npos) return "400 Bad Request";

    std::string path = first_line.substr(method_end + 1, path_end - method_end - 1);

    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    if (path.empty() || path == "/") {
        path = "/index.html";
    }

    return path;
}

bool is_safe_path(const std::string& path) {
    if (path.find("..") != std::string::npos) return false;
    if (path.find("\\") != std::string::npos) return false;
    return true;
}

std::string get_file_content(const std::string& path) {
    if (!is_safe_path(path)) {
        return "";
    }

    std::string fs_path = "./www" + path;

    std::ifstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void send_response(int client_fd, const std::string& content,
                   const std::string& content_type = "text/html",
                   const std::string& status = "200 OK") {
    std::string response = "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(content.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";

    write(client_fd, response.c_str(), response.size());
    if (!content.empty()) {
        write(client_fd, content.data(), content.size());
    }
}

void handle_http_client(int client_fd) {
    std::string path = parse_request(client_fd);

    if (path.empty()) {
        close(client_fd);
        return;
    }

    if (path == "400 Bad Request") {
        std::string body = "<h1>400 Bad Request</h1>";
        send_response(client_fd, body, "text/html", "400 Bad Request");
        close(client_fd);
        return;
    }

    std::string content = get_file_content(path);
    if (content.empty()) {
        std::string body = "<h1>404 Not Found</h1>";
        send_response(client_fd, body, "text/html", "404 Not Found");
    } else {
        std::string content_type = get_content_type(path);
        send_response(client_fd, content, content_type, "200 OK");
    }

    close(client_fd);
}

void worker_thread() {
    while (true) {
        int client_fd;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty() || stop_pool; });

            if (stop_pool && task_queue.empty()) {
                return;
            }

            client_fd = task_queue.front();
            task_queue.pop();
        }

        handle_http_client(client_fd);
    }
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 20) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back(worker_thread);
    }

    std::cout << "HTTP server listening on port " << port << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            task_queue.push(client_fd);
        }

        cv.notify_one();
    }

    close(server_fd);
    return 0;
}
