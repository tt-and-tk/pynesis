#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "lexer.hpp"

// 字句解析するファイル
typedef struct {
    std::string name;            // エラーメッセージに表示するファイル名
    std::string canonical_path;  // 同じファイルかどうかの判定に使う，正規化した絶対パス
    std::string text;            // ソース全体
} source_t;

// 1回のコンパイル全体で共有する字句解析の途中経過
// 字句解析と指令の処理は取り込むファイルごとに再帰呼び出しし合うため，共有する値を1つの引数にまとめて受け渡す
typedef struct {
    std::set<std::string> lexed_paths;        // 字句解析を始めたファイルの正規化した絶対パス
    std::set<std::string> in_progress_paths;  // 字句解析中のファイルの正規化した絶対パス
    std::set<std::string> once_paths;         // #pragma onceが書かれたファイルの正規化した絶対パス
} lex_state_t;

// 指令ごとの処理 (ソース中の読み取り位置posは，指令名の後ろの空白を読み飛ばした位置で受け取り，指令の直後まで読み進める)
typedef void (*directive_handler_t)(const source_t &source, int &pos, const loc_t &loc,
                                    lex_state_t &state, std::vector<token_t> &tokens);

// 前宣言 (ファイル内部でのみ使用)
static source_t read_source(const std::string &path, const loc_t *included_at);  // ファイルを読み込む
// ファイル1つを字句解析してトークン列の末尾へ追加する
static void lex_file(const source_t &source, lex_state_t &state, std::vector<token_t> &tokens);
// #で始まる指令を1つ読み，指令名に対応する処理を呼び出す
static void lex_directive(const source_t &source, int &pos, int line, int brace_depth, lex_state_t &state, std::vector<token_t> &tokens);
// #includeで指定されたファイルを字句解析してトークン列に展開する
static void lex_include(const source_t &source, int &pos, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens);
// #pragmaを処理する
static void lex_pragma(const source_t &source, int &pos, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens);
static bool is_at_decl_boundary(const std::vector<token_t> &tokens, int brace_depth);  // トークン列がグローバルスコープの宣言と宣言の間で終わっているかを返す
static void check_directive_line_end(const std::string &src, int pos, const loc_t &loc);  // 指令の後ろの同じ行に空白とコメント以外が無いことを確認する
static std::string read_word(const std::string &src, int &pos);  // 現在位置から識別子に使える文字の並びを読み進めて返す
static token_kind_t get_keyword_kind(const std::string &word);  // 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す

// キーワード文字列からトークン種別への変換表
const std::map<std::string, token_kind_t> g_keywords = {
    {"int",      TK_INT},      {"char",     TK_CHAR},      {"short",    TK_SHORT},
    {"void",     TK_VOID},     {"signed",   TK_SIGNED},    {"unsigned", TK_UNSIGNED},
    {"struct",   TK_STRUCT},   {"const",    TK_CONST},
    {"if",       TK_IF},       {"else",     TK_ELSE},      {"for",      TK_FOR},
    {"while",    TK_WHILE},    {"do",       TK_DO},
    {"switch",   TK_SWITCH},   {"case",     TK_CASE},      {"default",  TK_DEFAULT},
    {"break",    TK_BREAK},    {"continue", TK_CONTINUE},
    {"return",   TK_RETURN},   {"sizeof",   TK_SIZEOF},
    {"print",    TK_PRINT},    {"scan",     TK_SCAN},
    {"streq",    TK_STREQ},    {"strcopy",  TK_STRCOPY},
};

// 演算子・区切り文字文字列からトークン種別への変換表
const std::map<std::string, token_kind_t> g_operators = {
    {"<<=", TK_LSHIFT_ASSIGN}, {">>=", TK_RSHIFT_ASSIGN},
    {"==",  TK_EQEQ},    {"!=",  TK_NEQ},      {"<=",  TK_LEQ},       {">=",  TK_GEQ},
    {"<<",  TK_LSHIFT},  {">>",  TK_RSHIFT},   {"&&",  TK_AMPAMP},    {"||",  TK_PIPEPIPE},
    {"++",  TK_PLUSPLUS},{"--",  TK_MINUSMINUS},
    {"+=",  TK_PLUS_ASSIGN}, {"-=", TK_MINUS_ASSIGN}, {"*=", TK_STAR_ASSIGN},
    {"/=",  TK_SLASH_ASSIGN},{"%=", TK_PCT_ASSIGN},
    {"&=",  TK_AMP_ASSIGN},  {"|=", TK_PIPE_ASSIGN},  {"^=", TK_CARET_ASSIGN},
    {"+",   TK_PLUS},    {"-",   TK_MINUS},    {"*",   TK_STAR},      {"/",   TK_SLASH},
    {"%",   TK_PERCENT}, {"&",   TK_AMP},      {"|",   TK_PIPE},      {"^",   TK_CARET},
    {"~",   TK_TILDE},   {"!",   TK_BANG},     {"<",   TK_LT},        {">",   TK_GT},
    {"=",   TK_ASSIGN},  {"?",   TK_QUESTION}, {":",   TK_COLON},
    {"(",   TK_LPAREN},  {")",   TK_RPAREN},   {"{",   TK_LBRACE},    {"}",   TK_RBRACE},
    {"[",   TK_LBRACKET},  {"]",   TK_RBRACKET},
    {";",   TK_SEMICOLON},{",",  TK_COMMA},{".",  TK_DOT},
};

// 指令名から指令ごとの処理への変換表
const std::map<std::string, directive_handler_t> g_directives = {
    {"include", lex_include},
    {"pragma",  lex_pragma},
};

// 位置をエラーメッセージ用の「ファイル名:行番号」形式に変換する
std::string loc_to_string(const loc_t &loc) {
    return loc.file + ":" + std::to_string(loc.line);
}

// ファイルを読み込んで字句解析し，トークン列を生成する
void lex(const std::string &file_name, std::vector<token_t> &tokens) {
    lex_state_t state;  // 1回のコンパイル全体で共有する字句解析の途中経過

    // 指定されたファイルを読み込む
    const source_t source = read_source(file_name, nullptr);  // 指定されたファイル
    // 取り込むファイルを含めて字句解析する
    lex_file(source, state, tokens);

    // ファイル末尾トークンを，指定されたファイルの最終行の位置で追加する
    const int last_line = static_cast<int>(std::count(source.text.begin(), source.text.end(), '\n')) + 1;  // 最終行の行番号
    tokens.push_back({TK_EOF, "", {source.name, last_line}});
}

// ファイルを読み込み，表示用のファイル名・正規化した絶対パス・ソース全体を返す
// included_atは取り込み指令の位置 (取り込まれたファイルでなければnullptr)
static source_t read_source(const std::string &path, const loc_t *included_at) {
    const std::string at = included_at ? " at " + loc_to_string(*included_at) : "";  // エラーメッセージに付ける取り込み指令の位置
    source_t source;  // 読み込んだファイル

    // 表示用のファイル名は，.・..を解決し区切り文字をOSによらず/にそろえる
    source.name = std::filesystem::path(path).lexically_normal().generic_string();

    // ファイル全体を読み込む
    std::ifstream file(source.name);  // 読み込むファイル
    if (!file) {
        throw std::string("compiler error: cannot open file '") + source.name + "'" + at;
    }
    std::ostringstream ss;  // ファイル全体を文字列にするためのバッファ
    ss << file.rdbuf();
    source.text = ss.str();

    // 同じファイルを別の書き方のパスで指しても同一と判定できるよう，正規化した絶対パスを求める
    // (canonicalは失敗すると例外を投げ，compiler errorとして報告できないため，エラーコードを受け取る形で呼ぶ)
    std::error_code ec;  // 正規化に失敗した場合のエラー
    source.canonical_path = std::filesystem::canonical(source.name, ec).string();
    if (ec) {
        throw std::string("compiler error: cannot open file '") + source.name + "'" + at;
    }
    return source;
}

// ファイル1つを字句解析してトークン列の末尾へ追加する (#includeで取り込むファイルはその位置に展開する)
static void lex_file(const source_t &source, lex_state_t &state, std::vector<token_t> &tokens) {
    const std::string &src = source.text;  // ソース全体
    int pos  = 0;   // 現在の読み取り位置
    int line = 1;   // 現在の行番号
    int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    int brace_depth = 0;  // このファイル内の波括弧の入れ子の深さ (0ならグローバルスコープ直下)

    // 取り込みの重複・循環を検出できるよう，字句解析を始めたことを記録する
    state.lexed_paths.insert(source.canonical_path);
    state.in_progress_paths.insert(source.canonical_path);

    while (pos < src_size) {
        const char c = src[pos];  // 現在の文字

        // 改行: 行番号をインクリメントする
        if (c == '\n') { line++; pos++; continue; }

        // 空白文字: スキップする
        if (c == ' ' || c == '\t' || c == '\r') { pos++; continue; }

        // 行コメント (//): 行末までスキップする
        if (c == '/' && pos + 1 < src_size && src[pos + 1] == '/') {
            while (pos < src_size && src[pos] != '\n') pos++;
            // lineのインクリメントは外部whileループの次のループで行う
            continue;
        }

        // ブロックコメント (/* */): 閉じるまでスキップする
        if (c == '/' && pos + 1 < src_size && src[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < src_size) {
                if (src[pos] == '\n') line++;
                if (src[pos] == '*' && src[pos + 1] == '/') { pos += 2; break; }
                pos++;
            }
            continue;
        }

        // 指令: # で始まる
        if (c == '#') {
            lex_directive(source, pos, line, brace_depth, state, tokens);
            continue;
        }

        // 識別子または予約語: アルファベットか _ で始まる
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            // 識別子に使える文字の並びを読む
            const std::string word = read_word(src, pos);  // 識別子または予約語の文字列
            // 予約語ならその種別で，そうでなければ識別子としてトークンを追加する
            tokens.push_back({get_keyword_kind(word), word, {source.name, line}});
            continue;
        }

        // 整数リテラル: 数字で始まる
        if (isdigit(static_cast<unsigned char>(c))) {
            int start = pos;  // リテラルの先頭位置
            // 16進数 (0x...) の場合
            if (c == '0' && pos + 1 < src_size
                && (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
                pos += 2;
                while (pos < src_size
                       && isxdigit(static_cast<unsigned char>(src[pos]))) {
                    pos++;
                }
            }
            // 10進数の場合
            else {
                while (pos < src_size
                       && isdigit(static_cast<unsigned char>(src[pos]))) {
                    pos++;
                }
            }
            // 読み進めた範囲をトークンとして追加する
            tokens.push_back({TK_INT_LIT, src.substr(start, pos - start), {source.name, line}});
            continue;
        }

        // 文字リテラル: ' で始まる
        if (c == '\'') {
            int start = pos;  // リテラルの先頭位置
            pos++;  // 開き ' をスキップする
            // エスケープシーケンスの場合，バックスラッシュの次の文字を文字本体として扱う
            if (pos < src_size && src[pos] == '\\') pos++;
            // 改行は，エスケープの有無に関わらずエラーにする
            if (pos < src_size && src[pos] == '\n') {
                throw std::string("compiler error: newline in char literal at ")
                      + loc_to_string({source.name, line});
            }
            pos++;  // 文字本体をスキップする
            // 閉じ ' を確認する
            if (pos >= src_size || src[pos] != '\'') {
                throw std::string("compiler error: unterminated char literal at ")
                      + loc_to_string({source.name, line});
            }
            pos++;  // 閉じ ' をスキップする
            // 引用符を含めた範囲をトークンとして追加する
            tokens.push_back({TK_CHAR_LIT, src.substr(start, pos - start), {source.name, line}});
            continue;
        }

        // 文字列リテラル: " で始まる
        if (c == '"') {
            int start = pos;  // リテラルの先頭位置
            pos++;  // 開き " をスキップする
            // 閉じ " が来るまで読み進める (エスケープシーケンスを考慮する)
            while (pos < src_size && src[pos] != '"') {
                if (src[pos] == '\\') pos++;  // エスケープ対象の文字は閉じ " として扱わない
                // 改行は，エスケープの有無に関わらずエラーにする
                if (pos < src_size && src[pos] == '\n') {
                    throw std::string("compiler error: newline in string literal at ")
                          + loc_to_string({source.name, line});
                }
                pos++;
            }
            if (pos >= src_size) {
                throw std::string("compiler error: unterminated string literal at ")
                      + loc_to_string({source.name, line});
            }
            pos++;  // 閉じ " をスキップする
            // 引用符を除いた中身を取得する (エスケープシーケンスはパーサーで解釈する)
            const std::string content = src.substr(start + 1, pos - start - 2);  // 文字列リテラルの中身
            // 隣接する文字列リテラルを連結する ("hello" " world" → hello world)
            if (!tokens.empty() && tokens.back().kind == TK_STRING_LIT) {
                tokens.back().value += content;
            } else {
                tokens.push_back({TK_STRING_LIT, content, {source.name, line}});
            }
            continue;
        }

        // 演算子・区切り文字: 3文字→2文字→1文字の順に最長一致を試みる
        bool matched = false;  // 演算子・区切り文字に一致したか
        for (int len = 3; len >= 1; len--) {
            // ソースの末尾を越える長さは試さない
            if (pos + len > src_size) continue;
            const std::string token = src.substr(pos, len);  // 一致を試す文字列
            const auto it = g_operators.find(token);        // 一致した演算子・区切り文字
            if (it != g_operators.end()) {
                // 指令を書ける位置の判定に使うため，波括弧の入れ子の深さを数える
                if (it->second == TK_LBRACE) brace_depth++;
                if (it->second == TK_RBRACE) brace_depth--;
                // 一致した演算子・区切り文字をトークンとして追加する
                tokens.push_back({it->second, token, {source.name, line}});
                pos += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::string("compiler error: unknown character '")
                  + c + "' at " + loc_to_string({source.name, line});
        }
    }

    // ファイルの中で宣言が完結していることを確認する (取り込んだ先で別のファイルと宣言がつながらないようにする)
    if (!is_at_decl_boundary(tokens, brace_depth)) {
        throw std::string("compiler error: file ends in the middle of a declaration at ")
              + loc_to_string({source.name, line});
    }

    // 字句解析を終えたことを記録する
    state.in_progress_paths.erase(source.canonical_path);
}

// #で始まる指令を1つ読み，指令名に対応する処理を呼び出す
// ソース中の読み取り位置posは#の位置で受け取り，指令の直後まで読み進める
// brace_depthは指令を書いたファイル内の波括弧の入れ子の深さ
static void lex_directive(const source_t &source, int &pos, int line, int brace_depth, lex_state_t &state, std::vector<token_t> &tokens) {
    const std::string &src = source.text;               // ソース全体
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    const loc_t loc = {source.name, line};              // 指令の位置

    // 指令の#は行の1文字目に書く
    if (pos > 0 && src[pos - 1] != '\n') {
        throw std::string("compiler error: directive must be at the beginning of a line at ") + loc_to_string(loc);
    }

    // #の直後に続けて書かれた指令名を読み，対応する処理を探す
    pos++;
    const std::string name = read_word(src, pos);         // 指令名
    const auto directive = g_directives.find(name);     // 指令名に対応する処理
    if (directive == g_directives.end()) {
        throw std::string("compiler error: unknown directive '#") + name + "' at " + loc_to_string(loc);
    }

    // 指令はグローバルスコープの宣言と宣言の間にのみ書ける
    if (!is_at_decl_boundary(tokens, brace_depth)) {
        throw std::string("compiler error: '#") + name
              + "' is only allowed between global declarations at " + loc_to_string(loc);
    }

    // 指令名と引数の間の空白を読み飛ばす
    while (pos < src_size && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    // 指令ごとの処理を呼び出す
    directive->second(source, pos, loc, state, tokens);
}

// #includeの引数からパスを読み，そのファイルを字句解析してトークン列に展開する
static void lex_include(const source_t &source, int &pos, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens) {
    const std::string &src = source.text;               // ソース全体
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

    // 取り込むファイルのパスを"で囲んで読む (パスは行をまたげない)
    if (pos >= src_size || src[pos] != '"') {
        throw std::string("compiler error: expected \"file name\" after '#include' at ") + loc_to_string(loc);
    }
    const int start = ++pos;  // パスの先頭位置
    // 閉じ"か行末まで読み進める
    while (pos < src_size && src[pos] != '"' && src[pos] != '\n') pos++;
    if (pos >= src_size || src[pos] != '"') {
        throw std::string("compiler error: unterminated file name in '#include' at ") + loc_to_string(loc);
    }
    const std::string written = src.substr(start, pos - start);  // 取り込み指令に書かれたパス
    pos++;  // 閉じ " をスキップする
    // 指令の後ろに余分な記述が無いことを確認する
    check_directive_line_end(src, pos, loc);

    // 取り込むファイルもPynesisソースに限る
    if (written.length() < 3 || written.substr(written.length() - 3) != ".pn") {
        throw std::string("compiler error: included file '") + written
              + "' must have .pn extension at " + loc_to_string(loc);
    }

    // パスを取り込み指令が書かれたファイルがあるディレクトリから解決し，ファイルを読み込む
    const source_t included =   // 取り込むファイル
        read_source((std::filesystem::path(source.name).parent_path() / written).string(), &loc);

    // #pragma onceが書かれたファイルなら，2回目以降の取り込みは無視する
    if (state.once_paths.count(included.canonical_path)) {
        return;
    }
    // 字句解析中のファイルを取り込む場合 (#pragma onceより前の取り込み指令から循環した場合を含む)
    if (state.in_progress_paths.count(included.canonical_path)) {
        throw std::string("compiler error: circular include of '") + included.name + "' at " + loc_to_string(loc);
    }
    // 取り込み済みのファイルを#pragma onceなしで再び取り込む場合
    if (state.lexed_paths.count(included.canonical_path)) {
        throw std::string("compiler error: '") + included.name
              + "' is included more than once (add '#pragma once' to it) at " + loc_to_string(loc);
    }
    // 取り込むファイルを字句解析する
    lex_file(included, state, tokens);
}

// #pragmaの引数を読み，onceならそのファイルの2回目以降の取り込みを無視するよう記録する
static void lex_pragma(const source_t &source, int &pos, const loc_t &loc, lex_state_t &state, std::vector<token_t> &) {
    // pragmaの種類を読む (onceのみ対応する)
    const std::string pragma = read_word(source.text, pos);  // pragmaの種類
    if (pragma != "once") {
        throw std::string("compiler error: unknown pragma '") + pragma + "' at " + loc_to_string(loc);
    }
    // 指令の後ろに余分な記述が無いことを確認する
    check_directive_line_end(source.text, pos, loc);
    // このファイルに#pragma onceが書かれたことを記録する
    state.once_paths.insert(source.canonical_path);
}

// ここまでのトークン列が，グローバルスコープの宣言と宣言の間で終わっているかを返す
// (まだトークンが1つも無い最初の宣言の前，グローバルスコープ直下の;の直後，または関数本体を閉じる}の直後)
// 構造体定義の}は後ろに;が続き宣言の途中にあたるため，}は対応する{の直前が)である関数本体の場合に限る
static bool is_at_decl_boundary(const std::vector<token_t> &tokens, int brace_depth) {
    // 波括弧の中なら宣言の途中
    if (brace_depth != 0) return false;
    // まだトークンが1つも無い(コメントや指令だけが先にある)か，;の直後なら宣言の間
    if (tokens.empty() || tokens.back().kind == TK_SEMICOLON) return true;
    // ;と}以外の直後なら宣言の途中
    if (tokens.back().kind != TK_RBRACE) return false;

    // 末尾の}に対応する{を後ろから探す
    int depth = 0;  // 探索中の波括弧の入れ子の深さ
    for (int k = static_cast<int>(tokens.size()) - 1; k >= 0; k--) {
        if (tokens[k].kind == TK_RBRACE) depth++;
        if (tokens[k].kind == TK_LBRACE) depth--;
        // 対応する{が見つかったら，その直前が)なら関数本体
        if (depth == 0) {
            return k > 0 && tokens[k - 1].kind == TK_RPAREN;
        }
    }
    return false;
}

// 指令の後ろの同じ行に，空白とコメント以外が書かれていないことを確認する
// (ソース中の読み取り位置posは読み進めず，コメントは呼び出し元の字句解析で読み飛ばす)
static void check_directive_line_end(const std::string &src, int pos, const loc_t &loc) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    while (true) {
        // 空白を読み飛ばす
        while (pos < src_size && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r')) pos++;
        // 行末・ファイル末尾・行コメントに達したら，同じ行の残りに記述は無い
        if (pos >= src_size || src[pos] == '\n' || src.compare(pos, 2, "//") == 0) return;
        // コメント以外が書かれている場合
        if (src.compare(pos, 2, "/*") != 0) {
            throw std::string("compiler error: unexpected text after directive at ") + loc_to_string(loc);
        }
        // ブロックコメントの終わりを探す
        const size_t end = src.find("*/", pos + 2);   // ブロックコメントを閉じる*/の位置
        // 閉じずにファイルが終わるか，途中で改行して行をまたぐなら，同じ行の残りに記述は無い
        if (end == std::string::npos || src.find('\n', pos) < end) return;
        // 同じ行で閉じたブロックコメントの後ろも続けて調べる
        pos = static_cast<int>(end) + 2;
    }
}

// 現在位置から識別子に使える文字(英数字と_)の並びを読み進めて返す
static std::string read_word(const std::string &src, int &pos) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    const int start = pos;                                // 読み始めの位置
    // 英数字と_が続く限り読み進める
    while (pos < src_size
           && (isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) {
        pos++;
    }
    return src.substr(start, pos - start);
}

// 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す
static token_kind_t get_keyword_kind(const std::string &word) {
    const auto it = g_keywords.find(word);  // 識別子に一致したキーワード
    if (it != g_keywords.end()) return it->second;
    return TK_IDENT;
}
