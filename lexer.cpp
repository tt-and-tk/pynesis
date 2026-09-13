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
    // 取り込んだファイルが関数本体を開いたまま終わる場合も正しく数えられるよう，ファイルごとではなく全ファイルを通して数える
    int brace_depth = 0;                      // 波括弧の入れ子の深さ (0ならグローバルスコープ直下)
} lex_state_t;

// 指令ごとの処理 (位置iは指令名の後ろの空白を読み飛ばした位置で受け取り，指令の直後まで読み進める)
typedef void (*directive_handler_t)(const source_t &source, int &i, const loc_t &loc,
                                    lex_state_t &state, std::vector<token_t> &tokens);

// 前宣言 (ファイル内部でのみ使用)
static source_t read_source(const std::string &path, const loc_t *included_at);  // ファイルを読み込む
// ファイル1つを字句解析してトークン列の末尾へ追加する
static void lex_file(const source_t &source, lex_state_t &state, std::vector<token_t> &tokens);
// #で始まる指令を1つ読み，指令名に対応する処理を呼び出す
static void lex_directive(const source_t &source, int &i, int line, lex_state_t &state, std::vector<token_t> &tokens);
// #includeで指定されたファイルを字句解析してトークン列に展開する
static void lex_include(const source_t &source, int &i, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens);
// #pragmaを処理する
static void lex_pragma(const source_t &source, int &i, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens);
static bool is_at_decl_boundary(const std::vector<token_t> &tokens, int brace_depth);  // トークン列がグローバルスコープの宣言と宣言の間で終わっているかを返す
static void check_directive_line_end(const std::string &src, int i, const loc_t &loc);  // 指令の後ろの同じ行に空白とコメント以外が無いことを確認する
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
    int i    = 0;   // 現在の読み取り位置
    int line = 1;   // 現在の行番号
    int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

    // 取り込みの重複・循環を検出できるよう，字句解析を始めたことを記録する
    state.lexed_paths.insert(source.canonical_path);
    state.in_progress_paths.insert(source.canonical_path);

    while (i < src_size) {
        const char c = src[i];  // 現在の文字

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

        // 指令: # で始まる
        if (c == '#') {
            lex_directive(source, i, line, state, tokens);
            continue;
        }

        // 識別子または予約語: アルファベットか _ で始まる
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            // 識別子に使える文字の並びを読む
            const std::string word = read_word(src, i);  // 識別子または予約語の文字列
            // 予約語ならその種別で，そうでなければ識別子としてトークンを追加する
            tokens.push_back({get_keyword_kind(word), word, {source.name, line}});
            continue;
        }

        // 整数リテラル: 数字で始まる
        if (isdigit(static_cast<unsigned char>(c))) {
            int start = i;  // リテラルの先頭位置
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
            // 読み進めた範囲をトークンとして追加する
            tokens.push_back({TK_INT_LIT, src.substr(start, i - start), {source.name, line}});
            continue;
        }

        // 文字リテラル: ' で始まる
        if (c == '\'') {
            int start = i;  // リテラルの先頭位置
            i++;  // 開き ' をスキップする
            // エスケープシーケンスの場合，バックスラッシュの次の文字を文字本体として扱う
            if (i < src_size && src[i] == '\\') i++;
            // 改行は，エスケープの有無に関わらずエラーにする
            if (i < src_size && src[i] == '\n') {
                throw std::string("compiler error: newline in char literal at ")
                      + loc_to_string({source.name, line});
            }
            i++;  // 文字本体をスキップする
            // 閉じ ' を確認する
            if (i >= src_size || src[i] != '\'') {
                throw std::string("compiler error: unterminated char literal at ")
                      + loc_to_string({source.name, line});
            }
            i++;  // 閉じ ' をスキップする
            // 引用符を含めた範囲をトークンとして追加する
            tokens.push_back({TK_CHAR_LIT, src.substr(start, i - start), {source.name, line}});
            continue;
        }

        // 文字列リテラル: " で始まる
        if (c == '"') {
            int start = i;  // リテラルの先頭位置
            i++;  // 開き " をスキップする
            // 閉じ " が来るまで読み進める (エスケープシーケンスを考慮する)
            while (i < src_size && src[i] != '"') {
                if (src[i] == '\\') i++;  // エスケープ対象の文字は閉じ " として扱わない
                // 改行は，エスケープの有無に関わらずエラーにする
                if (i < src_size && src[i] == '\n') {
                    throw std::string("compiler error: newline in string literal at ")
                          + loc_to_string({source.name, line});
                }
                i++;
            }
            if (i >= src_size) {
                throw std::string("compiler error: unterminated string literal at ")
                      + loc_to_string({source.name, line});
            }
            i++;  // 閉じ " をスキップする
            // 引用符を除いた中身を取得する (エスケープシーケンスはパーサーで解釈する)
            const std::string content = src.substr(start + 1, i - start - 2);  // 文字列リテラルの中身
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
            if (i + len > src_size) continue;
            const std::string token = src.substr(i, len);  // 一致を試す文字列
            const auto it = g_operators.find(token);        // 一致した演算子・区切り文字
            if (it != g_operators.end()) {
                // 指令を書ける位置の判定に使うため，波括弧の入れ子の深さを数える
                if (it->second == TK_LBRACE) state.brace_depth++;
                if (it->second == TK_RBRACE) state.brace_depth--;
                // 一致した演算子・区切り文字をトークンとして追加する
                tokens.push_back({it->second, token, {source.name, line}});
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::string("compiler error: unknown character '")
                  + c + "' at " + loc_to_string({source.name, line});
        }
    }

    // 字句解析を終えたことを記録する
    state.in_progress_paths.erase(source.canonical_path);
}

// #で始まる指令を1つ読み，指令名に対応する処理を呼び出す
// 位置iは#の位置で受け取り，指令の直後まで読み進める
static void lex_directive(const source_t &source, int &i, int line, lex_state_t &state, std::vector<token_t> &tokens) {
    const std::string &src = source.text;               // ソース全体
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    const loc_t loc = {source.name, line};              // 指令の位置

    // 指令は行頭に書く (#の前の同じ行には空白だけを置ける)
    int before = i - 1;  // #の前の同じ行を後ろから調べる位置
    while (before >= 0 && (src[before] == ' ' || src[before] == '\t')) before--;
    if (before >= 0 && src[before] != '\n') {
        throw std::string("compiler error: directive must be at the beginning of a line at ") + loc_to_string(loc);
    }

    // #の直後に続けて書かれた指令名を読み，対応する処理を探す
    i++;
    const std::string name = read_word(src, i);         // 指令名
    const auto directive = g_directives.find(name);     // 指令名に対応する処理
    if (directive == g_directives.end()) {
        throw std::string("compiler error: unknown directive '#") + name + "' at " + loc_to_string(loc);
    }

    // 指令はグローバルスコープの宣言と宣言の間にのみ書ける
    if (!is_at_decl_boundary(tokens, state.brace_depth)) {
        throw std::string("compiler error: '#") + name
              + "' is only allowed between global declarations at " + loc_to_string(loc);
    }

    // 指令名と引数の間の空白を読み飛ばす
    while (i < src_size && (src[i] == ' ' || src[i] == '\t')) i++;
    // 指令ごとの処理を呼び出す
    directive->second(source, i, loc, state, tokens);
}

// #includeの引数からパスを読み，そのファイルを字句解析してトークン列に展開する
static void lex_include(const source_t &source, int &i, const loc_t &loc, lex_state_t &state, std::vector<token_t> &tokens) {
    const std::string &src = source.text;               // ソース全体
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ

    // 取り込むファイルのパスを"で囲んで読む (パスは行をまたげない)
    if (i >= src_size || src[i] != '"') {
        throw std::string("compiler error: expected \"file name\" after '#include' at ") + loc_to_string(loc);
    }
    const int start = ++i;  // パスの先頭位置
    // 閉じ"か行末まで読み進める
    while (i < src_size && src[i] != '"' && src[i] != '\n') i++;
    if (i >= src_size || src[i] != '"') {
        throw std::string("compiler error: unterminated file name in '#include' at ") + loc_to_string(loc);
    }
    const std::string written = src.substr(start, i - start);  // 取り込み指令に書かれたパス
    i++;  // 閉じ " をスキップする
    // 指令の後ろに余分な記述が無いことを確認する
    check_directive_line_end(src, i, loc);

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
static void lex_pragma(const source_t &source, int &i, const loc_t &loc, lex_state_t &state, std::vector<token_t> &) {
    // pragmaの種類を読む (onceのみ対応する)
    const std::string pragma = read_word(source.text, i);  // pragmaの種類
    if (pragma != "once") {
        throw std::string("compiler error: unknown pragma '") + pragma + "' at " + loc_to_string(loc);
    }
    // 指令の後ろに余分な記述が無いことを確認する
    check_directive_line_end(source.text, i, loc);
    // このファイルに#pragma onceが書かれたことを記録する
    state.once_paths.insert(source.canonical_path);
}

// ここまでのトークン列が，グローバルスコープの宣言と宣言の間で終わっているかを返す
// (プログラムの先頭，グローバルスコープ直下の;の直後，または関数本体を閉じる}の直後)
// 構造体定義の}は後ろに;が続き宣言の途中にあたるため，}は対応する{の直前が)である関数本体の場合に限る
static bool is_at_decl_boundary(const std::vector<token_t> &tokens, int brace_depth) {
    // 波括弧の中なら宣言の途中
    if (brace_depth != 0) return false;
    // プログラムの先頭，または;の直後なら宣言の間
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
// (位置iは読み進めず，コメントは呼び出し元の字句解析で読み飛ばす)
static void check_directive_line_end(const std::string &src, int i, const loc_t &loc) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    // 空白を読み飛ばす
    while (i < src_size && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) i++;
    const bool only_comment_follows =   // 行末・ファイル末尾・コメントの開始のいずれかか
        i >= src_size || src[i] == '\n'
        || (src[i] == '/' && i + 1 < src_size && (src[i + 1] == '/' || src[i + 1] == '*'));
    if (!only_comment_follows) {
        throw std::string("compiler error: unexpected text after directive at ") + loc_to_string(loc);
    }
}

// 現在位置から識別子に使える文字(英数字と_)の並びを読み進めて返す
static std::string read_word(const std::string &src, int &i) {
    const int src_size = static_cast<int>(src.size());  // ソース全体のサイズ
    const int start = i;                                // 読み始めの位置
    // 英数字と_が続く限り読み進める
    while (i < src_size
           && (isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
        i++;
    }
    return src.substr(start, i - start);
}

// 識別子がキーワードならその種別を，そうでなければ TK_IDENT を返す
static token_kind_t get_keyword_kind(const std::string &word) {
    const auto it = g_keywords.find(word);  // 識別子に一致したキーワード
    if (it != g_keywords.end()) return it->second;
    return TK_IDENT;
}
