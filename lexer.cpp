#include <cctype>
#include <map>

#include "lexer.hpp"

// 前宣言 (ファイル内部でのみ使用)
static token_kind_t get_keyword_kind(const std::string &word);  // 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す

// キーワード文字列からトークン種別への変換表
const std::map<std::string, token_kind_t> g_keywords = {
    {"int",      TK_INT},      {"char",     TK_CHAR},      {"short",    TK_SHORT},
    {"void",     TK_VOID},     {"signed",   TK_SIGNED},    {"unsigned", TK_UNSIGNED},
    {"struct",   TK_STRUCT},
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

// ソースコードをトークン列に変換する
void lex(const std::string &src, std::vector<token_t> &tokens) {
    int i    = 0;   // 現在の読み取り位置
    int line = 1;   // 現在の行番号
    int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

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

        // 識別子または予約語: アルファベットか _ で始まる
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            int start = i;
            while (i < src_size
                   && (isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
                i++;
            }
            const std::string word = src.substr(start, i - start);
            tokens.push_back({get_keyword_kind(word), word, line});
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
            tokens.push_back({TK_INT_LIT, src.substr(start, i - start), line});
            continue;
        }

        // 文字リテラル: ' で始まる
        if (c == '\'') {
            int start = i;
            i++;  // 開き ' をスキップする
            // エスケープシーケンスの場合，バックスラッシュの次の文字もスキップする
            if (i < src_size && src[i] == '\\') i++;
            i++;  // 文字本体をスキップする
            // 閉じ ' を確認する
            if (i >= src_size || src[i] != '\'') {
                throw std::string("compiler error: unterminated char literal at line ")
                      + std::to_string(line);
            }
            i++;  // 閉じ ' をスキップする
            tokens.push_back({TK_CHAR_LIT, src.substr(start, i - start), line});
            continue;
        }

        // 文字列リテラル: " で始まる
        if (c == '"') {
            int start = i;
            i++;  // 開き " をスキップする
            // 閉じ " が来るまで読み進める (エスケープシーケンスを考慮する)
            while (i < src_size && src[i] != '"') {
                if (src[i] == '\n') {
                    throw std::string("compiler error: newline in string literal at line ")
                          + std::to_string(line);
                }
                if (src[i] == '\\') i++;  // エスケープ文字の次をスキップする
                i++;
            }
            if (i >= src_size) {
                throw std::string("compiler error: unterminated string literal at line ")
                      + std::to_string(line);
            }
            i++;  // 閉じ " をスキップする
            // 引用符を除いた中身を取得する (エスケープシーケンスはパーサーで解釈する)
            const std::string content = src.substr(start + 1, i - start - 2);
            // 隣接する文字列リテラルを連結する ("hello" " world" → hello world)
            if (!tokens.empty() && tokens.back().kind == TK_STRING_LIT) {
                tokens.back().value += content;
            } else {
                tokens.push_back({TK_STRING_LIT, content, line});
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
                tokens.push_back({it->second, token, line});
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::string("compiler error: unknown character '")
                  + c + "' at line " + std::to_string(line);
        }
    }

    // ファイル末尾トークンを追加する
    tokens.push_back({TK_EOF, "", line});
}

// 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す
static token_kind_t get_keyword_kind(const std::string &word) {
    const auto it = g_keywords.find(word);
    if (it != g_keywords.end()) return it->second;
    return TK_IDENT;
}
