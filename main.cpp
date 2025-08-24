// library_lms.cpp
// Compile: g++ library_lms.cpp -o library_lms -std=c++17 -lsqlite3

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>
#include <sstream>
#include <sqlite3.h>
#include <cstdlib>
#include <cctype>

using namespace std;

const string DBFILE = "library.db";
const int FINE_PER_DAY = 2;
const int DEFAULT_BORROW_STUDENT = 14;
const int DEFAULT_BORROW_FACULTY = 30;
const int DEFAULT_BORROW_STAFF = 21;

sqlite3 *DB = nullptr;

// -------------------- SQLite helpers --------------------
static void die(const string &msg){
    cerr << msg << "\n";
    if(DB) sqlite3_close(DB);
    exit(1);
}

static string now_iso(){
    using namespace std::chrono;
    auto t = system_clock::now();
    time_t tt = system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm); // UTC
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return string(buf);
}

static string today_iso_date(){
    auto s = now_iso();
    return s.substr(0,10);
}

static string escape_sql(const string &s){
    string out; out.reserve(s.size()*2);
    for(char c: s){
        if(c=='\'') out += "''";
        else out.push_back(c);
    }
    return out;
}

static void exec_sql(const string &sql){
    char *err = nullptr;
    if (sqlite3_exec(DB, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK){
        string e = err? err : "Unknown sqlite error";
        sqlite3_free(err);
        die("SQL error: " + e + "\nWhen running: " + sql);
    }
}

static vector<vector<string>> query_sql(const string &sql){
    vector<vector<string>> rows;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(DB, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK){
        die("Failed to prepare query: " + sql);
    }
    int cols = sqlite3_column_count(stmt);
    while (true){
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW){
            vector<string> row;
            for (int i=0;i<cols;++i){
                const unsigned char *text = sqlite3_column_text(stmt, i);
                row.push_back(text? string((const char*)text) : string());
            }
            rows.push_back(row);
        } else if (rc == SQLITE_DONE){
            break;
        } else {
            sqlite3_finalize(stmt);
            die("Error stepping statement");
        }
    }
    sqlite3_finalize(stmt);
    return rows;
}

// -------------------- DB init & seed --------------------
static void init_db(){
    if (sqlite3_open(DBFILE.c_str(), &DB) != SQLITE_OK){
        die("Cannot open DB file: " + DBFILE);
    }

    // Create tables if not exist
    exec_sql(R"SQL(
    CREATE TABLE IF NOT EXISTS users (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        password TEXT NOT NULL,
        role TEXT NOT NULL,
        category TEXT
    );
    )SQL");

    exec_sql(R"SQL(
    CREATE TABLE IF NOT EXISTS books (
        book_id TEXT PRIMARY KEY,
        isbn TEXT,
        title TEXT NOT NULL,
        author TEXT,
        publisher TEXT,
        year INTEGER,
        rack TEXT,
        total_copies INTEGER NOT NULL DEFAULT 1,
        available_copies INTEGER NOT NULL DEFAULT 1,
        borrowed_count INTEGER NOT NULL DEFAULT 0
    );
    )SQL");

    exec_sql(R"SQL(
    CREATE TABLE IF NOT EXISTS transactions (
        txn_id TEXT PRIMARY KEY,
        member_id TEXT NOT NULL,
        book_id TEXT NOT NULL,
        issue_date TEXT NOT NULL,
        due_date TEXT NOT NULL,
        return_date TEXT,
        fine INTEGER DEFAULT 0,
        status TEXT NOT NULL,
        FOREIGN KEY(member_id) REFERENCES users(id),
        FOREIGN KEY(book_id) REFERENCES books(book_id)
    );
    )SQL");

    exec_sql(R"SQL(
    CREATE TABLE IF NOT EXISTS reservations (
        res_id INTEGER PRIMARY KEY AUTOINCREMENT,
        book_id TEXT NOT NULL,
        member_id TEXT NOT NULL,
        res_date TEXT NOT NULL,
        status TEXT NOT NULL
    );
    )SQL");

    // Seed default data only if users table empty
    auto rows = query_sql("SELECT COUNT(*) FROM users;");
    if (!rows.empty() && !rows[0].empty() && stoi(rows[0][0])==0){
        // insert admin, staff, member
        exec_sql("INSERT OR REPLACE INTO users (id,name,password,role,category) VALUES ('admin1','Library Admin','admin1','admin', NULL);");
        exec_sql("INSERT OR REPLACE INTO users (id,name,password,role,category) VALUES ('staff1','Librarian','staff1','staff', NULL);");
        exec_sql("INSERT OR REPLACE INTO users (id,name,password,role,category) VALUES ('m001','Alice Student','m001','member','student');");

        // sample books
        exec_sql("INSERT OR REPLACE INTO books (book_id,isbn,title,author,publisher,year,rack,total_copies,available_copies) VALUES \
            ('b001','9780131103627','The C Programming Language','Kernighan & Ritchie','Prentice Hall',1978,'R1-01',3,3);");
        exec_sql("INSERT OR REPLACE INTO books (book_id,isbn,title,author,publisher,year,rack,total_copies,available_copies) VALUES \
            ('b002','9780132350884','Clean Code','Robert C. Martin','Prentice Hall',2008,'R2-03',2,2);");
        exec_sql("INSERT OR REPLACE INTO books (book_id,isbn,title,author,publisher,year,rack,total_copies,available_copies) VALUES \
            ('b003','9780262033848','Introduction to Algorithms','Cormen et al.','MIT Press',2009,'R3-05',1,1);");
    }
}

// -------------------- Utilities --------------------
static string read_nonempty(const string &prompt){
    string s;
    do {
        cout << prompt;
        if(!getline(cin, s)) exit(0);
        // trim
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a==string::npos) s = "";
        else s = s.substr(a, s.find_last_not_of(" \t\r\n")-a+1);
    } while (s.empty());
    return s;
}

static string prompt(const string &p){
    cout << p;
    string s; getline(cin, s);
    return s;
}

static long days_between_dates_iso(const string &d1_iso, const string &d2_iso){
    // d1_iso, d2_iso: "YYYY-MM-DD..." we compare dates part
    tm t1{}; tm t2{};
    string a = d1_iso.substr(0,10);
    string b = d2_iso.substr(0,10);
    strptime(a.c_str(), "%Y-%m-%d", &t1);
    strptime(b.c_str(), "%Y-%m-%d", &t2);
    time_t tt1 = timegm(&t1);
    time_t tt2 = timegm(&t2);
    return (tt2 - tt1) / 86400;
}

// -------------------- Authentication --------------------
struct User {
    string id,name,role,category;
};

static bool login(User &user){
    cout << "\n--- Login ---\n";
    string uid = read_nonempty("User ID: ");
    string pwd = prompt("Password: ");
    string sql = "SELECT id,name,role,category FROM users WHERE id='" + escape_sql(uid) + "' AND password='" + escape_sql(pwd) + "';";
    auto rows = query_sql(sql);
    if (!rows.empty()){
        user.id = rows[0][0];
        user.name = rows[0][1];
        user.role = rows[0][2];
        user.category = rows[0][3];
        cout << "Welcome " << user.name << " (" << user.role << ")\n";
        return true;
    } else {
        cout << "Invalid credentials.\n";
        return false;
    }
}

// -------------------- Admin functions --------------------
static void list_books(){
    auto rows = query_sql("SELECT book_id,isbn,title,author,available_copies,total_copies FROM books;");
    cout << "\nBooks:\n";
    cout << left << setw(8) << "ID" << setw(18) << "ISBN" << setw(40) << "Title" << setw(20) << "Author" << setw(8) << "Avail" << setw(8) << "Total" << "\n";
    for(auto &r: rows){
        cout << setw(8) << r[0] << setw(18) << r[1] << setw(40) << (r[2].size()>38? r[2].substr(0,35)+"...": r[2]) << setw(20) << (r[3].size()>18? r[3].substr(0,17)+"...": r[3]) << setw(8) << r[4] << setw(8) << r[5] << "\n";
    }
}

static void add_book(){
    cout << "\n--- Add Book ---\n";
    string bid = read_nonempty("Book ID (unique): ");
    string title = read_nonempty("Title: ");
    string author = prompt("Author: ");
    string isbn = prompt("ISBN: ");
    string publisher = prompt("Publisher: ");
    string year = prompt("Year (YYYY): ");
    string rack = prompt("Rack No.: ");
    string copies_s = prompt("Copies (default 1): ");
    int copies = copies_s.empty()? 1 : stoi(copies_s);
    string sql = "INSERT OR REPLACE INTO books (book_id,isbn,title,author,publisher,year,rack,total_copies,available_copies) VALUES ('"
        + escape_sql(bid) + "','" + escape_sql(isbn) + "','" + escape_sql(title) + "','" + escape_sql(author) + "','" + escape_sql(publisher) + "',"
        + (year.empty()? "NULL": to_string(stoi(year))) + ",'" + escape_sql(rack) + "'," + to_string(copies) + "," + to_string(copies) + ");";
    exec_sql(sql);
    cout << "Book added/updated.\n";
}

static void update_book(){
    cout << "\n--- Update Book ---\n";
    string bid = read_nonempty("Book ID: ");
    auto rows = query_sql("SELECT book_id,title,author,total_copies,available_copies FROM books WHERE book_id='" + escape_sql(bid) + "';");
    if(rows.empty()){ cout << "Book not found.\n"; return; }
    auto &r = rows[0];
    cout << "Current Title: " << r[1] << " Author: " << r[2] << " Total: " << r[3] << " Avail: " << r[4] << "\n";
    string title = prompt("New Title (leave blank): ");
    string author = prompt("New Author (leave blank): ");
    string copies_txt = prompt("New total copies (leave blank): ");
    vector<string> updates;
    if(!title.empty()) updates.push_back("title='" + escape_sql(title) + "'");
    if(!author.empty()) updates.push_back("author='" + escape_sql(author) + "'");
    if(!copies_txt.empty()){
        int copies = stoi(copies_txt);
        // adjust available by difference
        int old_total = stoi(r[3]);
        int diff = copies - old_total;
        updates.push_back("total_copies=" + to_string(copies));
        updates.push_back("available_copies = available_copies + " + to_string(diff));
    }
    if(!updates.empty()){
        string sql = "UPDATE books SET " + accumulate(next(updates.begin()), updates.end(), updates[0],
            [](const string &a, const string &b){ return a + ", " + b; }) + " WHERE book_id='" + escape_sql(bid) + "';";
        exec_sql(sql);
        cout << "Updated.\n";
    } else cout << "Nothing changed.\n";
}

static void remove_book(){
    cout << "\n--- Remove Book ---\n";
    string bid = read_nonempty("Book ID: ");
    auto rows = query_sql("SELECT total_copies,available_copies FROM books WHERE book_id='" + escape_sql(bid) + "';");
    if(rows.empty()){ cout << "Book not found.\n"; return; }
    int total = stoi(rows[0][0]), avail = stoi(rows[0][1]);
    if(total != avail){ cout << "Cannot remove: some copies are borrowed.\n"; return; }
    exec_sql("DELETE FROM books WHERE book_id='" + escape_sql(bid) + "';");
    cout << "Removed.\n";
}

static void add_staff(){
    cout << "\n--- Add Staff ---\n";
    string sid = read_nonempty("Staff ID: ");
    string name = read_nonempty("Name: ");
    string pwd = prompt("Password: ");
    string sql = "INSERT OR REPLACE INTO users (id,name,password,role,category) VALUES ('" + escape_sql(sid) + "','" + escape_sql(name) + "','" + escape_sql(pwd) + "','staff', NULL);";
    exec_sql(sql);
    cout << "Staff added.\n";
}

static void list_users(){
    auto rows = query_sql("SELECT id,name,role,category FROM users;");
    cout << "\nUsers:\n";
    for(auto &r: rows) cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
}

// -------------------- Staff functions --------------------
static void add_member(){
    cout << "\n--- Add Member ---\n";
    string mid = read_nonempty("Member ID: ");
    string name = read_nonempty("Name: ");
    string category;
    while(true){
        category = prompt("Category (student/faculty/staff): ");
        for(auto &c: category) c = tolower(c);
        if(category=="student"||category=="faculty"||category=="staff") break;
        cout << "Invalid category\n";
    }
    string pwd = prompt("Password: ");
    string sql = "INSERT OR REPLACE INTO users (id,name,password,role,category) VALUES ('" + escape_sql(mid) + "','" + escape_sql(name) + "','" + escape_sql(pwd) + "','member','" + escape_sql(category) + "');";
    exec_sql(sql);
    cout << "Member added.\n";
}

static void list_members(){
    auto rows = query_sql("SELECT id,name,category FROM users WHERE role='member';");
    cout << "\nMembers:\n";
    for(auto &r: rows) cout << r[0] << " | " << r[1] << " | " << r[2] << "\n";
}

static void issue_book(){
    cout << "\n--- Issue Book ---\n";
    string mid = read_nonempty("Member ID: ");
    auto mrows = query_sql("SELECT id,category FROM users WHERE id='" + escape_sql(mid) + "' AND role='member';");
    if(mrows.empty()){ cout << "Member not found.\n"; return; }
    string cat = mrows[0][1];
    string bid = read_nonempty("Book ID: ");
    auto brows = query_sql("SELECT book_id,available_copies FROM books WHERE book_id='" + escape_sql(bid) + "';");
    if(brows.empty()){ cout << "Book not found.\n"; return; }
    int avail = stoi(brows[0][1]);
    if(avail < 1){ cout << "No copies available. Consider reserving.\n"; return; }

    // borrow limit
    auto rcnt = query_sql("SELECT COUNT(*) FROM transactions WHERE member_id='" + escape_sql(mid) + "' AND status='borrowed';");
    int borrowed_count = rcnt.empty()? 0 : stoi(rcnt[0][0]);
    int limit = 5;
    if(cat=="faculty") limit = 10;
    else if(cat=="staff") limit = 7;
    if(borrowed_count >= limit){ cout << "Borrow limit reached (" << limit << ")\n"; return; }

    // issue
    auto issue = now_iso();
    int days = (cat=="faculty"? DEFAULT_BORROW_FACULTY : (cat=="staff"? DEFAULT_BORROW_STAFF : DEFAULT_BORROW_STUDENT));
    // compute due date (YYYY-MM-DD)
    // For simplicity, compute by adding days to current time_t and format as ISO
    time_t t = time(nullptr);
    t += days * 24 * 3600;
    tm tm_due{}; gmtime_r(&t, &tm_due);
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_due);
    string due = string(buf);
    // txn id
    long long ts = chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    string txn = "TX" + to_string(ts);
    string sql = "INSERT INTO transactions (txn_id,member_id,book_id,issue_date,due_date,status) VALUES ('" + escape_sql(txn) + "','" + escape_sql(mid) + "','" + escape_sql(bid) + "','" + escape_sql(issue) + "','" + escape_sql(due) + "','borrowed');";
    exec_sql(sql);
    exec_sql("UPDATE books SET available_copies = available_copies - 1, borrowed_count = borrowed_count + 1 WHERE book_id='" + escape_sql(bid) + "';");
    cout << "Issued. TxnID=" << txn << " Due: " << due.substr(0,10) << "\n";
}

static void return_book(){
    cout << "\n--- Return Book ---\n";
    string txn = read_nonempty("Transaction ID: ");
    auto rows = query_sql("SELECT txn_id,member_id,book_id,issue_date,due_date,return_date,status FROM transactions WHERE txn_id='" + escape_sql(txn) + "';");
    if(rows.empty()){ cout << "Transaction not found.\n"; return; }
    auto &r = rows[0];
    if(r[6] == "returned"){ cout << "Already returned.\n"; return; }
    // compute fine
    string due = r[4];
    string ret = now_iso();
    // compute overdue days
    string due_date_only = due.substr(0,10);
    string ret_date_only = ret.substr(0,10);
    // Use tm parsing
    tm td{}; tm tr{};
    strptime(due_date_only.c_str(), "%Y-%m-%d", &td);
    strptime(ret_date_only.c_str(), "%Y-%m-%d", &tr);
    time_t tt_due = timegm(&td);
    time_t tt_ret = timegm(&tr);
    long overdue = 0;
    if (tt_ret > tt_due) overdue = (tt_ret - tt_due) / 86400;
    int fine = (overdue > 0) ? overdue * FINE_PER_DAY : 0;
    // update transaction
    string sql = "UPDATE transactions SET return_date='" + escape_sql(ret) + "', fine=" + to_string(fine) + ", status='returned' WHERE txn_id='" + escape_sql(txn) + "';";
    exec_sql(sql);
    // free book
    string bid = r[2];
    exec_sql("UPDATE books SET available_copies = available_copies + 1 WHERE book_id='" + escape_sql(bid) + "';");
    cout << "Book returned. Fine: ₹" << fine << "\n";

    // check reservations
    auto res = query_sql("SELECT res_id,member_id FROM reservations WHERE book_id='" + escape_sql(bid) + "' AND status='waiting' ORDER BY res_date LIMIT 1;");
    if(!res.empty()){
        string res_id = res[0][0];
        string next_member = res[0][1];
        exec_sql("UPDATE reservations SET status='fulfilled' WHERE res_id=" + res_id + ";");
        // auto-issue to next_member
        // determine category
        auto catRows = query_sql("SELECT category FROM users WHERE id='" + escape_sql(next_member) + "';");
        string cat = (catRows.empty() ? "student" : catRows[0][0]);
        int days = (cat=="faculty"? DEFAULT_BORROW_FACULTY : (cat=="staff"? DEFAULT_BORROW_STAFF : DEFAULT_BORROW_STUDENT));
        time_t tt = time(nullptr);
        tt += days * 24 * 3600;
        tm tm_due{}; gmtime_r(&tt, &tm_due);
        char buf2[32];
        strftime(buf2, sizeof(buf2), "%Y-%m-%dT%H:%M:%S", &tm_due);
        string due2 = string(buf2);
        long long ts2 = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
        string new_txn = "TX" + to_string(ts2);
        string issue_time = now_iso();
        string isql = "INSERT INTO transactions (txn_id,member_id,book_id,issue_date,due_date,status) VALUES ('" + escape_sql(new_txn) + "','" + escape_sql(next_member) + "','" + escape_sql(bid) + "','" + escape_sql(issue_time) + "','" + escape_sql(due2) + "','borrowed');";
        exec_sql(isql);
        exec_sql("UPDATE books SET available_copies = available_copies - 1, borrowed_count = borrowed_count + 1 WHERE book_id='" + escape_sql(bid) + "';");
        cout << "Reservation fulfilled: issued to " << next_member << " Txn " << new_txn << "\n";
    }
}

static void reserve_book(){
    cout << "\n--- Reserve Book ---\n";
    string mid = read_nonempty("Member ID: ");
    string bid = read_nonempty("Book ID: ");
    auto rows = query_sql("SELECT available_copies FROM books WHERE book_id='" + escape_sql(bid) + "';");
    if(rows.empty()){ cout << "Book not found.\n"; return; }
    int avail = stoi(rows[0][0]);
    if(avail > 0){ cout << "Book is available now; borrow instead.\n"; return; }
    string t = now_iso();
    exec_sql("INSERT INTO reservations (book_id,member_id,res_date,status) VALUES ('" + escape_sql(bid) + "','" + escape_sql(mid) + "','" + escape_sql(t) + "','waiting');");
    cout << "Reserved (FIFO). You'll be allocated when a copy is returned.\n";
}

static void list_borrowed(){
    auto rows = query_sql("SELECT txn_id,member_id,book_id,issue_date,due_date FROM transactions WHERE status='borrowed';");
    cout << "\nCurrently Borrowed:\n";
    for(auto &r: rows){
        cout << r[0] << " | Member:" << r[1] << " | Book:" << r[2] << " | Issue:" << r[3].substr(0,10) << " | Due:" << r[4].substr(0,10) << "\n";
    }
}

// -------------------- Member functions --------------------
static void search_books(){
    cout << "--- Search Books ---\n";
    string q = prompt("Query (title/author/isbn): ");
    string like = "%" + q + "%";
    // Using LIKE with escaped value (simple)
    auto rows = query_sql("SELECT book_id,isbn,title,author,available_copies FROM books WHERE title LIKE '%" + escape_sql(q) + "%' OR author LIKE '%" + escape_sql(q) + "%' OR isbn LIKE '%" + escape_sql(q) + "%';");
    cout << "\nSearch Results:\n";
    for(auto &r: rows) cout << r[0] << " | " << r[2] << " | " << r[3] << " | Avail:" << r[4] << "\n";
}

static void my_borrowed(const User &user){
    auto rows = query_sql("SELECT txn_id,book_id,issue_date,due_date,status,fine FROM transactions WHERE member_id='" + escape_sql(user.id) + "' ORDER BY issue_date DESC;");
    cout << "\nMy Transactions:\n";
    for(auto &r: rows){
        cout << r[0] << " | " << r[1] << " | Issue:" << r[2].substr(0,10) << " | Due:" << r[3].substr(0,10) << " | Status:" << r[4] << " | Fine:" << r[5] << "\n";
    }
}

static void return_book_member(const User &user){
    string txn = read_nonempty("Txn ID to return: ");
    // check ownership
    auto rows = query_sql("SELECT txn_id FROM transactions WHERE txn_id='" + escape_sql(txn) + "' AND member_id='" + escape_sql(user.id) + "' AND status='borrowed';");
    if(rows.empty()){ cout << "No matching borrowed transaction.\n"; return; }
    // reuse return_book
    return_book();
}

static void reserve_book_member(const User &user){
    string bid = read_nonempty("Book ID to reserve: ");
    auto rows = query_sql("SELECT available_copies FROM books WHERE book_id='" + escape_sql(bid) + "';");
    if(rows.empty()){ cout << "Book not found.\n"; return; }
    int avail = stoi(rows[0][0]);
    if(avail > 0){ cout << "Book available; you can borrow it instead.\n"; return; }
    string t = now_iso();
    exec_sql("INSERT INTO reservations (book_id,member_id,res_date,status) VALUES ('" + escape_sql(bid) + "','" + escape_sql(user.id) + "','" + escape_sql(t) + "','waiting');");
    cout << "Reserved. You'll be notified when available.\n";
}

// -------------------- Reports --------------------
static void report_overdue(){
    auto rows = query_sql("SELECT txn_id,member_id,book_id,issue_date,due_date FROM transactions WHERE status='borrowed';");
    cout << "\nOverdue:\n";
    for(auto &t: rows){
        string due = t[4];
        string now = now_iso();
        // compare dates:
        string due_date_only = due.substr(0,10);
        string now_date_only = now.substr(0,10);
        tm td{}; tm tn{};
        strptime(due_date_only.c_str(), "%Y-%m-%d", &td);
        strptime(now_date_only.c_str(), "%Y-%m-%d", &tn);
        time_t tt_due = timegm(&td);
        time_t tt_now = timegm(&tn);
        if(tt_now > tt_due){
            long overdue_days = (tt_now - tt_due) / 86400;
            long fine = overdue_days * FINE_PER_DAY;
            cout << "Txn:" << t[0] << " Member:" << t[1] << " Book:" << t[2] << " Due:" << due.substr(0,10) << " Days:" << overdue_days << " Fine:₹" << fine << "\n";
        }
    }
}

static void report_top_borrowed(){
    auto rows = query_sql("SELECT book_id,title,borrowed_count FROM books ORDER BY borrowed_count DESC LIMIT 10;");
    cout << "\nTop Borrowed Books:\n";
    for(auto &r: rows) cout << r[0] << " | " << r[1] << " | Count:" << r[2] << "\n";
}

// -------------------- Menus --------------------
static void admin_menu(const User &user){
    while(true){
        cout << R"(
--- Admin Menu ---
1) Add Book
2) Update Book
3) Remove Book
4) List Books
5) Add Staff
6) List Users
7) Reports
0) Logout
)";
        string ch = prompt("Choice: ");
        if(ch=="1") add_book();
        else if(ch=="2") update_book();
        else if(ch=="3") remove_book();
        else if(ch=="4") list_books();
        else if(ch=="5") add_staff();
        else if(ch=="6") list_users();
        else if(ch=="7"){
            while(true){
                cout << "Reports: 1) Overdue 2) Top Borrowed 0) Back\n";
                string r = prompt("Choice: ");
                if(r=="1") report_overdue();
                else if(r=="2") report_top_borrowed();
                else if(r=="0") break;
            }
        }
        else if(ch=="0") break;
    }
}

static void staff_menu(const User &user){
    while(true){
        cout << R"(
--- Staff Menu ---
1) Add Member
2) List Members
3) Issue Book
4) Return Book
5) Reserve Book (for member)
6) Borrowed List
7) Reports
0) Logout
)";
        string ch = prompt("Choice: ");
        if(ch=="1") add_member();
        else if(ch=="2") list_members();
        else if(ch=="3") issue_book();
        else if(ch=="4") return_book();
        else if(ch=="5") reserve_book();
        else if(ch=="6") list_borrowed();
        else if(ch=="7"){
            cout << "Reports: 1) Overdue 2) Top Borrowed 0) Back\n";
            string r = prompt("Choice: ");
            if(r=="1") report_overdue();
            else if(r=="2") report_top_borrowed();
        }
        else if(ch=="0") break;
    }
}

static void member_menu(const User &user){
    while(true){
        cout << R"(
--- Member Menu ---
1) Search Books
2) My Borrowed Books
3) Return Book (by TxnID)
4) Reserve Book
0) Logout
)";
        string ch = prompt("Choice: ");
        if(ch=="1") search_books();
        else if(ch=="2") my_borrowed(user);
        else if(ch=="3") return_book_member(user);
        else if(ch=="4") reserve_book_member(user);
        else if(ch=="0") break;
    }
}

// -------------------- Main --------------------
int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    init_db();

    cout << "=====================================\n  IITK - Campus Library Management\n=====================================\n";
    while(true){
        User user;
        if(!login(user)){
            string t = prompt("Try again? (y/n): ");
            if(t != "y") break;
            else continue;
        }
        if(user.role == "admin") admin_menu(user);
        else if(user.role == "staff") staff_menu(user);
        else if(user.role == "member") member_menu(user);
        else cout << "Unknown role\n";

        cout << "Logged out.\n";
        string again = prompt("Login as another user? (y/n): ");
        if(again != "y") break;
    }

    if(DB) sqlite3_close(DB);
    cout << "Goodbye.\n";
    return 0;
}
