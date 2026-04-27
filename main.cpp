#include "crow.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <random>

using json = nlohmann::json;

// ============================================================
//  CONFIG
// ============================================================
const std::string OPENAI_API_KEY = "YOUR_OPENAI_API_KEY_HERE";
const std::string DB_PATH        = "bookstore.db";

// ============================================================
//  DATABASE HELPER
// ============================================================
sqlite3* db = nullptr;

bool db_exec(const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB ERROR] " << errMsg << "\n";
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ============================================================
//  DATABASE INIT
// ============================================================
void init_database() {
    sqlite3_open(DB_PATH.c_str(), &db);

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS users (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            username    TEXT UNIQUE NOT NULL,
            password    TEXT NOT NULL,
            email       TEXT UNIQUE NOT NULL,
            xp          INTEGER DEFAULT 0,
            level       INTEGER DEFAULT 1,
            discount_pct INTEGER DEFAULT 0,
            created_at  TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS categories (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS books (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            title       TEXT NOT NULL,
            author      TEXT NOT NULL,
            type        TEXT CHECK(type IN ('manga','novel','webtoon','book')) DEFAULT 'book',
            category_id INTEGER REFERENCES categories(id),
            rent_price  REAL DEFAULT 0.0,
            buy_price   REAL DEFAULT 0.0,
            cover_url   TEXT,
            amazon_url  TEXT,
            description TEXT,
            created_at  TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS purchases (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id    INTEGER REFERENCES users(id),
            book_id    INTEGER REFERENCES books(id),
            type       TEXT CHECK(type IN ('rent','buy')) NOT NULL,
            price_paid REAL NOT NULL,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS reading_history (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id    INTEGER REFERENCES users(id),
            book_id    INTEGER REFERENCES books(id),
            progress   INTEGER DEFAULT 0,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS quizzes (
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            book_id INTEGER REFERENCES books(id),
            question TEXT NOT NULL,
            answer   TEXT NOT NULL,
            xp_reward INTEGER DEFAULT 50
        );
    )");

    db_exec(R"(
        CREATE TABLE IF NOT EXISTS discount_cards (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id    INTEGER REFERENCES users(id),
            discount   INTEGER NOT NULL,
            expires_at TEXT,
            used       INTEGER DEFAULT 0
        );
    )");

    // Seed categories
    db_exec("INSERT OR IGNORE INTO categories(name) VALUES ('Fantasy'),('Romance'),('Sci-Fi'),('Horror'),('Action');");

    // Seed sample books
    db_exec(R"(
        INSERT OR IGNORE INTO books(id,title,author,type,category_id,rent_price,buy_price,description)
        VALUES
        (1,'One Piece','Eiichiro Oda','manga',4,2.99,9.99,'Legendary pirate adventure'),
        (2,'Solo Leveling','Chugong','webtoon',4,1.99,7.99,'S-rank hunter story'),
        (3,'Dune','Frank Herbert','novel',3,3.99,12.99,'Sci-fi epic'),
        (4,'Berserk','Kentaro Miura','manga',4,2.99,9.99,'Dark fantasy manga');
    )");

    std::cout << "[DB] Database initialized.\n";
}

// ============================================================
//  CURL HELPER (for OpenAI)
// ============================================================
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string http_post(const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    std::string response;
    if (!curl) return "";

    struct curl_slist* hlist = nullptr;
    for (auto& h : headers) hlist = curl_slist_append(hlist, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    curl_easy_perform(curl);
    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return response;
}

// ============================================================
//  OPENAI CHAT
// ============================================================
std::string openai_chat(const std::string& system_prompt, const std::string& user_msg) {
    json payload = {
        {"model", "gpt-3.5-turbo"},
        {"messages", {
            {{"role","system"}, {"content", system_prompt}},
            {{"role","user"},   {"content", user_msg}}
        }},
        {"max_tokens", 512}
    };

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " + OPENAI_API_KEY
    };

    std::string raw = http_post("https://api.openai.com/v1/chat/completions",
                                payload.dump(), headers);
    try {
        auto j = json::parse(raw);
        return j["choices"][0]["message"]["content"].get<std::string>();
    } catch (...) {
        return "AI cavab verə bilmədi.";
    }
}

// ============================================================
//  XP + LEVEL + DISCOUNT CARD LOGIC
// ============================================================
void add_xp(int user_id, int xp_amount) {
    // Update XP
    std::string sql = "UPDATE users SET xp = xp + " + std::to_string(xp_amount) +
                      " WHERE id = " + std::to_string(user_id) + ";";
    db_exec(sql);

    // Recalculate level (every 500 XP = 1 level)
    db_exec("UPDATE users SET level = (xp / 500) + 1 WHERE id = " + std::to_string(user_id) + ";");

    // Check purchase count for discount cards
    sqlite3_stmt* stmt;
    std::string count_sql = "SELECT COUNT(*) FROM purchases WHERE user_id = " + std::to_string(user_id) + ";";
    sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        // Every 5 purchases → issue 10% discount card
        if (count % 5 == 0) {
            std::string card_sql = "INSERT INTO discount_cards(user_id, discount, expires_at) VALUES(" +
                std::to_string(user_id) + ", 10, date('now','+30 days'));";
            db_exec(card_sql);
        }
    }
    sqlite3_finalize(stmt);
}

// ============================================================
//  AI RECOMMENDER (content-based filtering)
// ============================================================
std::string get_recommendations(int user_id) {
    // Get user's reading history categories
    sqlite3_stmt* stmt;
    std::string sql = R"(
        SELECT b.title, b.author, b.type, c.name
        FROM reading_history rh
        JOIN books b ON b.id = rh.book_id
        JOIN categories c ON c.id = b.category_id
        WHERE rh.user_id = )" + std::to_string(user_id) + " LIMIT 5;";

    std::string history_text = "";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        history_text += std::string((char*)sqlite3_column_text(stmt, 0)) + " (" +
                        std::string((char*)sqlite3_column_text(stmt, 3)) + "), ";
    }
    sqlite3_finalize(stmt);

    if (history_text.empty()) history_text = "No history yet.";

    std::string system_prompt = "You are a book recommender. Based on reading history, suggest 3 books with title, author and a short reason. Be concise.";
    std::string user_msg = "User has read: " + history_text + ". Suggest 3 new books.";

    return openai_chat(system_prompt, user_msg);
}

// ============================================================
//  WATERMARK HELPER (DRM)
// ============================================================
std::string apply_watermark(int user_id, const std::string& content) {
    return content + "\n\n[Protected - User#" + std::to_string(user_id) +
           " | " + std::to_string(std::time(nullptr)) + "]";
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    init_database();
    curl_global_init(CURL_GLOBAL_ALL);

    crow::SimpleApp app;

    // ── Health Check ────────────────────────────────────────
    CROW_ROUTE(app, "/")([]{
        return crow::response(200, R"({"status":"BookStore API running"})");
    });

    // ── USERS ───────────────────────────────────────────────

    // Register
    CROW_ROUTE(app, "/api/register").methods("POST"_method)
    ([](const crow::request& req){
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return crow::response(400, R"({"error":"Invalid JSON"})");

        std::string username = body.value("username", "");
        std::string password = body.value("password", "");
        std::string email    = body.value("email", "");

        if (username.empty() || password.empty() || email.empty())
            return crow::response(400, R"({"error":"username, password, email required"})");

        std::string sql = "INSERT INTO users(username,password,email) VALUES('" +
                          username + "','" + password + "','" + email + "');";
        if (!db_exec(sql))
            return crow::response(409, R"({"error":"Username or email already exists"})");

        return crow::response(201, R"({"message":"User registered successfully"})");
    });

    // Login
    CROW_ROUTE(app, "/api/login").methods("POST"_method)
    ([](const crow::request& req){
        auto body = json::parse(req.body, nullptr, false);
        std::string username = body.value("username", "");
        std::string password = body.value("password", "");

        sqlite3_stmt* stmt;
        std::string sql = "SELECT id, level, xp, discount_pct FROM users WHERE username='" +
                          username + "' AND password='" + password + "';";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json resp = {
                {"message", "Login successful"},
                {"user_id", sqlite3_column_int(stmt, 0)},
                {"level",   sqlite3_column_int(stmt, 1)},
                {"xp",      sqlite3_column_int(stmt, 2)},
                {"discount_pct", sqlite3_column_int(stmt, 3)}
            };
            sqlite3_finalize(stmt);
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(401, R"({"error":"Invalid credentials"})");
    });

    // Get user profile
    CROW_ROUTE(app, "/api/user/<int>")([](int user_id){
        sqlite3_stmt* stmt;
        std::string sql = "SELECT username, email, xp, level, discount_pct FROM users WHERE id=" +
                          std::to_string(user_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json resp = {
                {"username",     (char*)sqlite3_column_text(stmt, 0)},
                {"email",        (char*)sqlite3_column_text(stmt, 1)},
                {"xp",           sqlite3_column_int(stmt, 2)},
                {"level",        sqlite3_column_int(stmt, 3)},
                {"discount_pct", sqlite3_column_int(stmt, 4)}
            };
            sqlite3_finalize(stmt);
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"User not found"})");
    });

    // ── BOOKS ────────────────────────────────────────────────

    // List all books (with optional ?type= filter)
    CROW_ROUTE(app, "/api/books")([](const crow::request& req){
        std::string filter = "";
        auto type_param = req.url_params.get("type");
        if (type_param) filter = " WHERE b.type='" + std::string(type_param) + "'";

        sqlite3_stmt* stmt;
        std::string sql = R"(
            SELECT b.id, b.title, b.author, b.type, c.name, b.rent_price, b.buy_price, b.cover_url, b.amazon_url, b.description
            FROM books b LEFT JOIN categories c ON c.id = b.category_id)" + filter + ";";

        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        json arr = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            arr.push_back({
                {"id",          sqlite3_column_int(stmt, 0)},
                {"title",       sqlite3_column_text(stmt, 1) ? (char*)sqlite3_column_text(stmt, 1) : ""},
                {"author",      sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : ""},
                {"type",        sqlite3_column_text(stmt, 3) ? (char*)sqlite3_column_text(stmt, 3) : ""},
                {"category",    sqlite3_column_text(stmt, 4) ? (char*)sqlite3_column_text(stmt, 4) : ""},
                {"rent_price",  sqlite3_column_double(stmt, 5)},
                {"buy_price",   sqlite3_column_double(stmt, 6)},
                {"cover_url",   sqlite3_column_text(stmt, 7) ? (char*)sqlite3_column_text(stmt, 7) : ""},
                {"amazon_url",  sqlite3_column_text(stmt, 8) ? (char*)sqlite3_column_text(stmt, 8) : ""},
                {"description", sqlite3_column_text(stmt, 9) ? (char*)sqlite3_column_text(stmt, 9) : ""}
            });
        }
        sqlite3_finalize(stmt);
        return crow::response(200, arr.dump());
    });

    // Get single book
    CROW_ROUTE(app, "/api/books/<int>")([](int book_id){
        sqlite3_stmt* stmt;
        std::string sql = "SELECT b.id, b.title, b.author, b.type, c.name, b.rent_price, b.buy_price, b.cover_url, b.amazon_url, b.description FROM books b LEFT JOIN categories c ON c.id=b.category_id WHERE b.id=" + std::to_string(book_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json resp = {
                {"id",          sqlite3_column_int(stmt, 0)},
                {"title",       sqlite3_column_text(stmt, 1) ? (char*)sqlite3_column_text(stmt, 1) : ""},
                {"author",      sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : ""},
                {"type",        sqlite3_column_text(stmt, 3) ? (char*)sqlite3_column_text(stmt, 3) : ""},
                {"category",    sqlite3_column_text(stmt, 4) ? (char*)sqlite3_column_text(stmt, 4) : ""},
                {"rent_price",  sqlite3_column_double(stmt, 5)},
                {"buy_price",   sqlite3_column_double(stmt, 6)},
                {"cover_url",   sqlite3_column_text(stmt, 7) ? (char*)sqlite3_column_text(stmt, 7) : ""},
                {"amazon_url",  sqlite3_column_text(stmt, 8) ? (char*)sqlite3_column_text(stmt, 8) : ""},
                {"description", sqlite3_column_text(stmt, 9) ? (char*)sqlite3_column_text(stmt, 9) : ""}
            };
            sqlite3_finalize(stmt);
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"Book not found"})");
    });

    // Add book (admin)
    CROW_ROUTE(app, "/api/books").methods("POST"_method)
    ([](const crow::request& req){
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return crow::response(400, R"({"error":"Invalid JSON"})");

        std::string title   = body.value("title", "");
        std::string author  = body.value("author", "");
        std::string type    = body.value("type", "book");
        int cat_id          = body.value("category_id", 1);
        double rent_price   = body.value("rent_price", 0.0);
        double buy_price    = body.value("buy_price", 0.0);
        std::string desc    = body.value("description", "");
        std::string amazon  = body.value("amazon_url", "");
        std::string cover   = body.value("cover_url", "");

        std::string sql = "INSERT INTO books(title,author,type,category_id,rent_price,buy_price,description,amazon_url,cover_url) VALUES('" +
            title + "','" + author + "','" + type + "'," + std::to_string(cat_id) + "," +
            std::to_string(rent_price) + "," + std::to_string(buy_price) + ",'" +
            desc + "','" + amazon + "','" + cover + "');";

        if (!db_exec(sql))
            return crow::response(500, R"({"error":"Failed to add book"})");

        return crow::response(201, R"({"message":"Book added successfully"})");
    });

    // ── PURCHASE / RENT ──────────────────────────────────────
    CROW_ROUTE(app, "/api/purchase").methods("POST"_method)
    ([](const crow::request& req){
        auto body = json::parse(req.body, nullptr, false);
        int user_id   = body.value("user_id", 0);
        int book_id   = body.value("book_id", 0);
        std::string type = body.value("type", "buy"); // rent or buy

        // Get book price
        sqlite3_stmt* stmt;
        std::string sql = "SELECT rent_price, buy_price FROM books WHERE id=" + std::to_string(book_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        double price = 0.0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            price = (type == "rent") ? sqlite3_column_double(stmt, 0) : sqlite3_column_double(stmt, 1);
        } else {
            sqlite3_finalize(stmt);
            return crow::response(404, R"({"error":"Book not found"})");
        }
        sqlite3_finalize(stmt);

        // Apply discount
        sql = "SELECT discount_pct FROM users WHERE id=" + std::to_string(user_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        int disc = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) disc = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        double final_price = price * (1.0 - disc / 100.0);

        // Record purchase
        sql = "INSERT INTO purchases(user_id,book_id,type,price_paid) VALUES(" +
              std::to_string(user_id) + "," + std::to_string(book_id) + ",'" +
              type + "'," + std::to_string(final_price) + ");";
        db_exec(sql);

        // Add reading history
        sql = "INSERT OR IGNORE INTO reading_history(user_id,book_id) VALUES(" +
              std::to_string(user_id) + "," + std::to_string(book_id) + ");";
        db_exec(sql);

        // Grant XP
        add_xp(user_id, 100);

        json resp = {
            {"message",     "Purchase successful"},
            {"type",        type},
            {"price_paid",  final_price},
            {"discount_pct", disc},
            {"xp_earned",   100}
        };
        return crow::response(200, resp.dump());
    });

    // ── QUIZ ─────────────────────────────────────────────────

    // Get quiz for book
    CROW_ROUTE(app, "/api/quiz/<int>")([](int book_id){
        sqlite3_stmt* stmt;
        std::string sql = "SELECT id, question FROM quizzes WHERE book_id=" + std::to_string(book_id) + " ORDER BY RANDOM() LIMIT 1;";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json resp = {
                {"quiz_id",  sqlite3_column_int(stmt, 0)},
                {"question", (char*)sqlite3_column_text(stmt, 1)}
            };
            sqlite3_finalize(stmt);
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"No quiz found for this book"})");
    });

    // Submit quiz answer
    CROW_ROUTE(app, "/api/quiz/answer").methods("POST"_method)
    ([](const crow::request& req){
        auto body    = json::parse(req.body, nullptr, false);
        int quiz_id  = body.value("quiz_id", 0);
        int user_id  = body.value("user_id", 0);
        std::string answer = body.value("answer", "");

        sqlite3_stmt* stmt;
        std::string sql = "SELECT answer, xp_reward FROM quizzes WHERE id=" + std::to_string(quiz_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string correct = (char*)sqlite3_column_text(stmt, 0);
            int xp_reward = sqlite3_column_int(stmt, 1);
            sqlite3_finalize(stmt);

            // Simple case-insensitive check
            std::transform(answer.begin(), answer.end(), answer.begin(), ::tolower);
            std::transform(correct.begin(), correct.end(), correct.begin(), ::tolower);

            if (answer == correct) {
                add_xp(user_id, xp_reward);
                json resp = {{"correct", true}, {"xp_earned", xp_reward}, {"message", "Correct answer! XP added."}};
                return crow::response(200, resp.dump());
            } else {
                json resp = {{"correct", false}, {"message", "Wrong answer. Try again!"}};
                return crow::response(200, resp.dump());
            }
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"Quiz not found"})");
    });

    // ── DISCOUNT CARDS ───────────────────────────────────────
    CROW_ROUTE(app, "/api/discount-cards/<int>")([](int user_id){
        sqlite3_stmt* stmt;
        std::string sql = "SELECT id, discount, expires_at, used FROM discount_cards WHERE user_id=" +
                          std::to_string(user_id) + " AND used=0;";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        json arr = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            arr.push_back({
                {"card_id",    sqlite3_column_int(stmt, 0)},
                {"discount",   sqlite3_column_int(stmt, 1)},
                {"expires_at", sqlite3_column_text(stmt, 2) ? (char*)sqlite3_column_text(stmt, 2) : ""},
                {"used",       sqlite3_column_int(stmt, 3)}
            });
        }
        sqlite3_finalize(stmt);
        return crow::response(200, arr.dump());
    });

    // Use a discount card
    CROW_ROUTE(app, "/api/discount-cards/use").methods("POST"_method)
    ([](const crow::request& req){
        auto body   = json::parse(req.body, nullptr, false);
        int card_id = body.value("card_id", 0);
        int user_id = body.value("user_id", 0);

        sqlite3_stmt* stmt;
        std::string sql = "SELECT discount FROM discount_cards WHERE id=" + std::to_string(card_id) +
                          " AND user_id=" + std::to_string(user_id) + " AND used=0;";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int disc = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);

            db_exec("UPDATE discount_cards SET used=1 WHERE id=" + std::to_string(card_id) + ";");
            db_exec("UPDATE users SET discount_pct=" + std::to_string(disc) +
                    " WHERE id=" + std::to_string(user_id) + ";");

            json resp = {{"message","Discount card applied"}, {"discount_pct", disc}};
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"Card not found or already used"})");
    });

    // ── AI RECOMMENDER ───────────────────────────────────────
    CROW_ROUTE(app, "/api/recommend/<int>")([](int user_id){
        std::string result = get_recommendations(user_id);
        json resp = {{"user_id", user_id}, {"recommendations", result}};
        return crow::response(200, resp.dump());
    });

    // ── AI CHATBOT ───────────────────────────────────────────
    CROW_ROUTE(app, "/api/chat").methods("POST"_method)
    ([](const crow::request& req){
        auto body = json::parse(req.body, nullptr, false);
        std::string message = body.value("message", "");
        int user_id = body.value("user_id", 0);

        if (message.empty())
            return crow::response(400, R"({"error":"message required"})");

        std::string system_prompt =
            "You are a helpful book discussion assistant for a digital bookstore. "
            "You help users with book recommendations, plot discussions, author info, "
            "and anything related to books, manga, webtoons, and light novels. "
            "Be friendly and enthusiastic about books.";

        std::string reply = openai_chat(system_prompt, message);
        json resp = {{"reply", reply}, {"user_id", user_id}};
        return crow::response(200, resp.dump());
    });

    // ── DRM - Read book with watermark ───────────────────────
    CROW_ROUTE(app, "/api/read/<int>/<int>")([](int user_id, int book_id){
        // Check if user purchased/rented this book
        sqlite3_stmt* stmt;
        std::string sql = "SELECT id FROM purchases WHERE user_id=" + std::to_string(user_id) +
                          " AND book_id=" + std::to_string(book_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return crow::response(403, R"({"error":"Access denied. Please purchase or rent this book first."})");
        }
        sqlite3_finalize(stmt);

        // Get book content (description as sample)
        sql = "SELECT title, author, description FROM books WHERE id=" + std::to_string(book_id) + ";";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string title = (char*)sqlite3_column_text(stmt, 0);
            std::string author = (char*)sqlite3_column_text(stmt, 1);
            std::string content = (char*)sqlite3_column_text(stmt, 2);
            sqlite3_finalize(stmt);

            // Apply DRM watermark
            std::string watermarked = apply_watermark(user_id, content);

            json resp = {
                {"title",   title},
                {"author",  author},
                {"content", watermarked},
                {"drm",     true},
                {"note",    "Screenshot blocking and clipboard lock must be enforced on client side."}
            };
            return crow::response(200, resp.dump());
        }
        sqlite3_finalize(stmt);
        return crow::response(404, R"({"error":"Book not found"})");
    });

    // ── LEADERBOARD ──────────────────────────────────────────
    CROW_ROUTE(app, "/api/leaderboard")([]{
        sqlite3_stmt* stmt;
        std::string sql = "SELECT username, level, xp FROM users ORDER BY xp DESC LIMIT 10;";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

        json arr = json::array();
        int rank = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            arr.push_back({
                {"rank",     rank++},
                {"username", (char*)sqlite3_column_text(stmt, 0)},
                {"level",    sqlite3_column_int(stmt, 1)},
                {"xp",       sqlite3_column_int(stmt, 2)}
            });
        }
        sqlite3_finalize(stmt);
        return crow::response(200, arr.dump());
    });

    // ── CATEGORIES ───────────────────────────────────────────
    CROW_ROUTE(app, "/api/categories")([]{
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "SELECT id, name FROM categories;", -1, &stmt, nullptr);
        json arr = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            arr.push_back({
                {"id",   sqlite3_column_int(stmt, 0)},
                {"name", (char*)sqlite3_column_text(stmt, 1)}
            });
        }
        sqlite3_finalize(stmt);
        return crow::response(200, arr.dump());
    });

    std::cout << "[SERVER] BookStore API starting on port 8080...\n";
    app.port(8080).multithreaded().run();

    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
