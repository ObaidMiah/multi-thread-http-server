#include <sys/socket.h>
#include <cstdio>
#include <netinet/in.h>
#include <string>
#include <unistd.h>
#include <thread>
#include <sstream>
#include <unordered_map>
#include <fstream>
#include <functional>
#include <sys/time.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <csignal>
#include <ctime>
#include <chrono>

using std::condition_variable;
using std::function;
using std::getline;
using std::ifstream;
using std::istringstream;
using std::mutex;
using std::queue;
using std::string;
using std::stringstream;
using std::thread;
using std::unordered_map;
using std::vector;

std::atomic<bool> running{true};
int listen_fd = -1; // global so the signal handler can close it

constexpr int WORKER_COUNT = 8;
queue<int> client_queue;
mutex queue_mutex;
condition_variable queue_cv;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024; // 1 MB

using RouteHandler = function<void(
    int,                                  // client fd
    const string &,                       // method
    const string &,                       // path
    const string &,                       // body
    const unordered_map<string, string> & // headers
    )>;

unordered_map<string, RouteHandler> routes;

// send the entire buffer, looping until all bytes are written
bool send_all(int client, const string &data)
{
    size_t total = 0;
    while (total < data.size())
    {
        ssize_t n = send(client, data.c_str() + total, data.size() - total, 0);
        if (n <= 0)
            return false; // client gone or error
        total += static_cast<size_t>(n);
    }
    return true;
}

// generic error handlers
void send_error(int client, int code, const string &message)
{
    string body = message + "\n";
    string response =
        "HTTP/1.1 " +
        std::to_string(code) +
        " " +
        message +
        "\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    send_all(client, response);
}

// generic response handler
void send_response(int client, const string &body, const string &status = "200 OK", const string &type = "text/plain", const string &conn = "keep-alive")
{
    string response =
        "HTTP/1.1 " +
        status +
        "\r\n"
        "Content-Type: " +
        type +
        "\r\n"
        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n"
        "Connection: " +
        conn +
        "\r\n\r\n" +
        body;

    if (!send_all(client, response))
    {
        printf("Failed to send to Client %d \n", client);
    }
    else
    {
        printf("Response sent to Client %d \n", client);
    }
}

// escape a string for safe embedding in JSON
string json_escape(const string &s)
{
    string out;
    out.reserve(s.size());

    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }

    return out;
}

// generic json handler
void send_json(int client, const string &json, const string &status = "200 OK", const string &conn = "keep-alive")
{
    send_response(
        client,
        json,
        status,
        "application/json",
        conn);
}

// mime map
string get_content_type(const string &path)
{
    if (path.ends_with(".html"))
        return "text/html";
    if (path.ends_with(".css"))
        return "text/css";
    if (path.ends_with(".js"))
        return "application/javascript";
    if (path.ends_with(".png"))
        return "image/png";
    if (path.ends_with(".jpg"))
        return "image/jpeg";
    if (path.ends_with(".jpeg"))
        return "image/jpeg";
    if (path.ends_with(".gif"))
        return "image/gif";
    return "application/octet-stream";
}

// generic file handler
void serve_static(int client, const string &path, const string &conn)
{
    std::string filepath = "www";

    if (path == "/")
        filepath += "/index.html";
    else
        filepath += path;

    ifstream f(filepath);
    stringstream file_content;

    if (f.is_open())
    {
        file_content << f.rdbuf();
    }
    else
    {
        send_error(client, 404, "Not Found");
        return;
    }

    string response_str = file_content.str();

    send_response(client, response_str, "200 OK", get_content_type(filepath), conn);
}

// parse request header
unordered_map<string, string> parse_headers(const string &header_string)
{
    unordered_map<string, string> headers;

    istringstream iss(header_string);
    string cur_line;

    // request line
    getline(iss, cur_line);

    // parse headers
    while (getline(iss, cur_line))
    {
        // end of header
        if (cur_line == "\r" || cur_line.empty())
        {
            break;
        }

        // remove \r
        if (!cur_line.empty() && cur_line.back() == '\r')
        {
            cur_line.pop_back();
        }

        // parse
        size_t colon = cur_line.find(':');
        if (colon == string::npos)
        {
            continue;
        }

        string key = cur_line.substr(0, colon);
        string value = cur_line.substr(colon + 2);

        headers[key] = value;
    }

    return headers;
}

// main packet processing
void handle_client(int client_connection)
{
    // receive timeout
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client_connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // read incoming message
    string request;
    char buffer[4096];
    bool open_connection = true;

    while (open_connection)
    {
        // clear request string
        request.clear();

        while (true)
        {
            ssize_t bytes_received = recv(client_connection, buffer, sizeof(buffer), 0);

            if (bytes_received <= 0)
            {
                break;
            }

            request.append(buffer, bytes_received);

            // reach end of http request
            if (request.find("\r\n\r\n") != std::string::npos)
            {
                break;
            }
        }

        // if no request was received (timeout or disconnect), close connection
        if (request.empty())
        {
            break;
        }

        // parse the http request
        size_t header_end = request.find("\r\n\r\n");
        if (header_end == string::npos)
        {
            send_error(client_connection, 400, "Bad Request");
            break;
        }
        string header = request.substr(0, header_end);
        string body = request.substr(header_end + 4);

        unordered_map<string, string> headers = parse_headers(header);

        size_t pos = header.find("\r\n");
        if (pos == string::npos)
        {
            send_error(client_connection, 400, "Bad Request");
            break;
        }
        string request_line = header.substr(0, pos);

        istringstream iss(request_line);
        string method, path, version;
        iss >> method;
        iss >> path;
        iss >> version;

        // strip path from any query
        size_t qpos = path.find('?');
        if (qpos != string::npos)
        {
            path = path.substr(0, qpos);
        }

        // check if valid request
        if (method.empty() || path.empty() || version.empty())
        {
            send_error(client_connection, 400, "Bad Request");
            break;
        }

        if (path.find("..") != string::npos) // malicious path
        {
            send_error(client_connection, 400, "Bad Request");
            break;
        }

        if (method != "GET" && method != "POST")
        {
            send_error(client_connection, 405, "Method Not Allowed");
            break;
        }

        // read in rest of body
        size_t body_len = 0;
        if (headers.find("Content-Length") != headers.end())
        {
            try
            {
                body_len = stoi(headers["Content-Length"]);

                if (body_len > MAX_BODY_SIZE)
                {
                    send_error(client_connection, 413, "Oversized Payload");
                    break;
                }
            }
            catch (...)
            {
                send_error(client_connection, 400, "Bad Request");
                break;
            }
        }

        while (body.size() < body_len)
        {
            ssize_t bytes_received = recv(client_connection, buffer, sizeof(buffer), 0);

            if (bytes_received <= 0)
            {
                break;
            }

            body.append(buffer, bytes_received);
        }

        // check default connection behavior
        if (version == "HTTP/1.1")
            open_connection = true;
        else
            open_connection = false;

        // override connection with any header value
        if (headers.count("Connection"))
        {
            if (headers["Connection"] == "close")
            {
                open_connection = false;
            }
            else if (headers["Connection"] == "keep-alive")
            {
                open_connection = true;
            }
        }

        // serve the http response
        try
        {
            string conn = open_connection ? "keep-alive" : "close";

            if (routes.count(path))
            {
                routes[path](client_connection, method, path, body, headers);
            }
            else
            {
                serve_static(client_connection, path, conn);
            }
        }
        catch (...)
        {
            send_error(client_connection, 500, "Internal Server Error");
            break;
        }
    }

    close(client_connection);
}

void worker_thread()
{
    while (true)
    {
        int client_connection;

        {
            std::unique_lock<mutex> lock(queue_mutex);
            queue_cv.wait(lock, []
                          { return !client_queue.empty() || !running; });

            if (!running && client_queue.empty())
                return; // drain done, exit thread

            client_connection = client_queue.front();
            client_queue.pop();
        }

        handle_client(client_connection);
    }
}

void handle_sigint(int)
{
    running = false;
    if (listen_fd != -1)
        close(listen_fd); // unblocks accept()
}

int main(int argc, char *argv[])
{
    // routes
    routes["/api/health"] = [](int client, const string &method, const string &, const string &, const auto &)
    {
        if (method != "GET")
        {
            send_error(client, 405, "Method Not Allowed");
            return;
        }

        send_json(client, R"({"status":"ok"})");
    };

    routes["/api/time"] = [](int client, const string &method, const string &, const string &, const auto &)
    {
        if (method != "GET")
        {
            send_error(client, 405, "Method Not Allowed");
            return;
        }

        time_t now = time(nullptr);
        send_json(client, string("{\"epoch\":") + std::to_string(now) + "}");
    };

    routes["/api/echo"] = [](int client, const string &method, const string &, const string &body, const auto &)
    {
        if (method != "POST")
        {
            send_error(client, 405, "Method Not Allowed");
            return;
        }

        send_json(client, string("{\"echo\":\"") + json_escape(body) + "\"}");
    };

    // create socket
    int httpServer = socket(AF_INET, SOCK_STREAM, 0);

    if (httpServer < 0)
    {
        printf("Failed to create socket \n");
        return -1;
    }

    // bind to address / port
    int port = 8080; 
    if(argc > 1)
        port = atoi(argv[1]); 

    sockaddr_in address{}; // need to zero
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    int bindStatus = bind(httpServer, (sockaddr *)&address, sizeof(address));

    if (bindStatus < 0)
    {
        printf("Failed to bind socket \n");
        return -1;
    }

    // listen
    int listenStatus = listen(httpServer, SOMAXCONN);

    if (listenStatus < 0)
    {
        printf("Listening error \n");
        return -1;
    }

    vector<thread> workers;

    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        workers.emplace_back(worker_thread);
    }

    listen_fd = httpServer;
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN); // don't die if a client disconnects mid-send

    while (running)
    {
        // accept
        int clientConnection = accept(httpServer, nullptr, nullptr);

        if (clientConnection < 0)
        {
            if (!running)
                break; // socket closed by shutdown
            printf("Accept failure");
            continue;
        }

        {
            std::lock_guard<mutex> lock(queue_mutex);
            client_queue.push(clientConnection);
        }

        queue_cv.notify_one();
    }

    // shutdown: wake all workers so they can drain the queue and exit
    queue_cv.notify_all();

    for (auto &w : workers)
        w.join();

    printf("Server shut down cleanly\n");
    return 0;
}
