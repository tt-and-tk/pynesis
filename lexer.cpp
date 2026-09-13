#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "lexer.hpp"

// 取り込み指令の処理状況 (1回のコンパイル全体で共有する)
// ファイルは正規化した絶対パスで識別する (同じファイルを別の書き方のパスで指しても同一と判定するため)
typedef struct {
    std::set<std::string> lexed;        // 字句解析を始めたファイル
    std::set<std::string> in_progress;  // 字句解析中のファイル (取り込みの循環を検出する)
    std::set<std::string> once;         // #pragma onceが書かれたファイル
} include_state_t;

// 前宣言 (ファイル内部でのみ使用)
// ファイル全体を読み込み，内容と識別用の正規化した絶対パスを返す
static void read_source(const std::string &file_name, const loc_t *included_at,
                        std::string &src, std::string &key);
// 1ファイル分のソースを字句解析してトークン列の末尾へ追加し，ファイル末尾の行番号を返す
static int lex_source(const std::string &src, const std::string &file_name, const std::string &key,
                      include_state_t &state, std::vector<token_t> &tokens);
// #で始まる指令を読み，#includeならファイルを展開し，#pragma onceなら記録する
static void lex_directive(const std::string &src, int &i, const loc_t &loc, int depth,
                          const std::string &key, include_state_t &state, std::vector<token_t> &tokens);
static std::string read_word(const std::string &src, int &i);  // 現在位置から識別子に使える文字の並びを読み進めて返す
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

// 位置をエラーメッセージ用の「ファイル名:行番号」形式に変換する
std::string loc_to_string(const loc_t &loc) {
    return loc.file + ":" + std::to_string(loc.line);
}

// ファイルを読み込んで字句解析し，トークン列を生成する
void lex(const std::string &file_name, std::vector<token_t> &tokens) {
    // エラーメッセージのファイル名は，OSによらず区切り文字を/にそろえて表示する
    const std::string name = std::filesystem::path(file_name).lexically_normal().generic_string();
    std::string src;            // ソース全体
    std::string key;            // 識別用の正規化した絶対パス
    include_state_t state;      // 取り込み指令の処理状況
    read_source(name, nullptr, src, key);
    const int last_line = lex_source(src, name, key, state, tokens);

    // ファイル末尾トークンを追加する
    tokens.push_back({TK_EOF, "", {name, last_line}});
}

// ファイル全体を読み込み，内容と識別用の正規化した絶対パスを返す
// included_atは取り込み指令の位置 (-pnで指定したファイルならnullptr)
static void read_source(const std::string &file_name, const loc_t *included_at,
                        std::string &src, std::string &key) {
    const std::string at = included_at ? " at " + loc_to_string(*included_at) : "";  // エラー位置の表記

    std::ifstream file(file_name);
    if (!file) {
        throw std::string("compiler error: cannot open file '") + file_name + "'" + at;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    src = ss.str();

    // 例外を投げる版はcompiler errorとして報告できないため，エラーコードを受け取る版を使う
    std::error_code ec;
    key = std::filesystem::canonical(file_name, ec).string();
    if (ec) {
        throw std::string("compiler error: cannot open file '") + file_name + "'" + at;
    }
}

// 1ファイル分のソースを字句解析してトークン列の末尾へ追加し，ファイル末尾の行番号を返す
static int lex_source(const std::string &src, const std::string &file_name, const std::string &key,
                      include_state_t &state, std::vector<token_t> &tokens) {
    int i    = 0;   // 現在の読み取り位置
    int line = 1;   // 現在の行番号
    int depth = 0;  // 波括弧の入れ子の深さ (0ならグローバルスコープ直下)
    int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

    // 取り込みの重複・循環を検出できるよう，字句解析を始めたことを記録する
    state.lexed.insert(key);
    state.in_progress.insert(key);

    while (i < src_size) {
        const char c = src[i];

        // 改行: 行番号をインクリメントする
        if (c == '\n') { line++; i++; continue; }

        // 空白文字: スキップする
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        // 行コメント (//): 行末までスキップする
        if (c == '/' && i + 1 < src_size && src[i + 1] == '/') {
            while (i < src_size && src[i] != '\n') i++;
            // lineのインクリメントは外部whileループの次のループで行う
            continue;
        }

        // ブロックコメント (/* */): 閉じるまでスキップする
        if (c == '/' && i + 1 < src_size && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src_size) {
                if (src[i] == '\n') line++;
                if (src[i] == '*' && src[i + 1] == '/') { i += 2; break; }
                i++;
            }
            continue;
        }

        // 指令: # で始まる (指令の後ろの同じ行の残りは，通常どおり字句解析を続ける)
        if (c == '#') {
            lex_directive(src, i, {file_name, line}, depth, key, state, tokens);
            continue;
        }

        // 識別子または予約語: アルファベットか _ で始まる
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::string word = read_word(src, i);
            tokens.push_back({get_keyword_kind(word), word, {file_name, line}});
            continue;
        }

        // 整数リテラル: 数字で始まる
        if (isdigit(static_cast<unsigned char>(c))) {
            int start = i;
            // 16進数 (0x...) の場合
            if (c == '0' && i + 1 < src_size
                && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < src_size
                       && isxdigit(static_cast<unsigned char>(src[i]))) {
                    i++;
                }
            }
            // 10進数の場合
            else {
                while (i < src_size
                       && isdigit(static_cast<unsigned char>(src[i]))) {
                    i++;
                }
            }
            tokens.push_back({TK_INT_LIT, src.substr(start, i - start), {file_name, line}});
            continue;
        }

        // 文字リテラル: ' で始まる
        if (c == '\'') {
            int start = i;
            i++;  // 開き ' をスキップする
            // エスケープシーケンスの場合，バックスラッシュの次の文字を文字本体として扱う
            if (i < src_size && src[i] == '\\') i++;
            // 改行は，エスケープの有無に関わらずエラーにする
            if (i < src_size && src[i] == '\n') {
                throw std::string("compiler error: newline in char literal at ")
                      + loc_to_string({file_name, line});
            }
            i++;  // 文字本体をスキップする
            // 閉じ ' を確認する
            if (i >= src_size || src[i] != '\'') {
                throw std::string("compiler error: unterminated char literal at ")
                      + loc_to_string({file_name, line});
            }
            i++;  // 閉じ ' をスキップする
            tokens.push_back({TK_CHAR_LIT, src.substr(start, i - start), {file_name, line}});
            continue;
        }

        // 文字列リテラル: " で始まる
        if (c == '"') {
            int start = i;
            i++;  // 開き " をスキップする
            // 閉じ " が来るまで読み進める (エスケープシーケンスを考慮する)
            while (i < src_size && src[i] != '"') {
                if (src[i] == '\\') i++;  // エスケープ対象の文字は閉じ " として扱わない
                // 改行は，エスケープの有無に関わらずエラーにする
                if (i < src_size && src[i] == '\n') {
                    throw std::string("compiler error: newline in string literal at ")
                          + loc_to_string({file_name, line});
                }
                i++;
            }
            if (i >= src_size) {
                throw std::string("compiler error: unterminated string literal at ")
                      + loc_to_string({file_name, line});
            }
            i++;  // 閉じ " をスキップする
            // 引用符を除いた中身を取得する (エスケープシーケンスはパーサーで解釈する)
            const std::string content = src.substr(start + 1, i - start - 2);
            // 隣接する文字列リテラルを連結する ("hello" " world" → hello world)
            if (!tokens.empty() && tokens.back().kind == TK_STRING_LIT) {
                tokens.back().value += content;
            } else {
                tokens.push_back({TK_STRING_LIT, content, {file_name, line}});
            }
            continue;
        }

        // 演算子・区切り文字: 3文字→2文字→1文字の順に最長一致を試みる
        bool matched = false;
        for (int len = 3; len >= 1; len--) {
            if (i + len > src_size) continue;
            const std::string token = src.substr(i, len);
            const auto it = g_operators.find(token);
            if (it != g_operators.end()) {
                // 指令をグローバルスコープ直下に限定するため，波括弧の入れ子の深さを数える
                if (it->second == TK_LBRACE) depth++;
                if (it->second == TK_RBRACE) depth--;
                tokens.push_back({it->second, token, {file_name, line}});
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::string("compiler error: unknown character '")
                  + c + "' at " + loc_to_string({file_name, line});
        }
    }

    state.in_progress.erase(key);
    return line;
}

// #で始まる指令を読み，#includeならファイルを展開し，#pragma onceなら記録する
// iは#の位置で呼び出し，指令の直後まで進める
static void lex_directive(const std::string &src, int &i, const loc_t &loc, int depth,
                          const std::string &key, include_state_t &state, std::vector<token_t> &tokens) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

    // 指令名は#の直後に続けて書く
    i++;
    const std::string name = read_word(src, i);  // 指令名
    if (name != "include" && name != "pragma") {
        throw std::string("compiler error: unknown directive '#") + name + "' at " + loc_to_string(loc);
    }
    // 取り込んだ内容はグローバル宣言として扱い，重複や循環の判定もグローバル宣言が順序によらず参照できることを前提にしているため，
    // 関数本体や構造体定義の中には書けない
    if (depth != 0) {
        throw std::string("compiler error: '#") + name + "' is only allowed at global scope at "
              + loc_to_string(loc);
    }

    // 指令名と引数の間の空白を読み飛ばす
    while (i < src_size && (src[i] == ' ' || src[i] == '\t')) i++;

    // #pragma: onceのみ対応する
    if (name == "pragma") {
        const std::string pragma = read_word(src, i);  // pragmaの種類
        if (pragma != "once") {
            throw std::string("compiler error: unknown pragma '") + pragma + "' at " + loc_to_string(loc);
        }
        state.once.insert(key);
        return;
    }

    // #include: 取り込むファイルのパスを"で囲んで読む (パスは行をまたげない)
    if (i >= src_size || src[i] != '"') {
        throw std::string("compiler error: expected \"file name\" after '#include' at ") + loc_to_string(loc);
    }
    const int start = ++i;  // パスの先頭位置
    while (i < src_size && src[i] != '"' && src[i] != '\n') i++;
    if (i >= src_size || src[i] != '"') {
        throw std::string("compiler error: unterminated file name in '#include' at ") + loc_to_string(loc);
    }
    const std::string written = src.substr(start, i - start);  // 取り込み指令に書かれたパス
    i++;  // 閉じ " をスキップする

    // 取り込むファイルもPynesisソースに限る
    if (written.length() < 3 || written.substr(written.length() - 3) != ".pn") {
        throw std::string("compiler error: included file '") + written
              + "' must have .pn extension at " + loc_to_string(loc);
    }

    // パスは取り込み指令を書いたファイルがあるディレクトリから解決する
    const std::string path =
        (std::filesystem::path(loc.file).parent_path() / written).lexically_normal().generic_string();
    std::string src_included;   // 取り込むファイルのソース全体
    std::string key_included;   // 取り込むファイルの識別用の正規化した絶対パス
    read_source(path, &loc, src_included, key_included);

    // #pragma onceが書かれたファイルなら，2回目以降の取り込みは無視する
    if (state.once.count(key_included)) {
        return;
    }
    // 字句解析中のファイルを取り込む場合 (#pragma onceより前の取り込み指令から循環した場合を含む)
    if (state.in_progress.count(key_included)) {
        throw std::string("compiler error: circular include of '") + path + "' at " + loc_to_string(loc);
    }
    // 取り込み済みのファイルを#pragma onceなしで再び取り込む場合
    if (state.lexed.count(key_included)) {
        throw std::string("compiler error: '") + path
              + "' is included more than once (add '#pragma once' to it) at " + loc_to_string(loc);
    }
    lex_source(src_included, path, key_included, state, tokens);
}

// 現在位置から識別子に使える文字(英数字と_)の並びを読み進めて返す
static std::string read_word(const std::string &src, int &i) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    const int start = i;                                // 読み始めの位置
    while (i < src_size
           && (isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
        i++;
    }
    return src.substr(start, i - start);
}

// 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す
static token_kind_t get_keyword_kind(const std::string &word) {
    const auto it = g_keywords.find(word);
    if (it != g_keywords.end()) return it->second;
    return TK_IDENT;
}
