#pragma once
#include <string>
#include <vector>

// トークン種別
typedef enum {
    // 型キーワード
    TK_INT, TK_CHAR, TK_SHORT, TK_VOID, TK_SIGNED, TK_UNSIGNED,
    TK_STRUCT,
    // 制御構文キーワード
    TK_IF, TK_ELSE, TK_FOR, TK_WHILE, TK_DO,
    TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_BREAK, TK_CONTINUE, TK_RETURN,
    // その他キーワード
    TK_SIZEOF,
    // 組み込み関数キーワード (ユーザー定義の変数・関数名との衝突を防ぐため予約語にする)
    TK_PRINT, TK_SCAN,
    // リテラル
    TK_INT_LIT,     // 整数リテラル
    TK_CHAR_LIT,    // 文字リテラル
    TK_STRING_LIT,  // 文字列リテラル
    // 識別子
    TK_IDENT,
    // 算術演算子
    TK_PLUS,        // +
    TK_MINUS,       // -
    TK_STAR,        // *
    TK_SLASH,       // /
    TK_PERCENT,     // %
    // ビット演算子
    TK_AMP,         // &
    TK_PIPE,        // |
    TK_CARET,       // ^
    TK_TILDE,       // ~
    TK_LSHIFT,      // <<
    TK_RSHIFT,      // >>
    // 論理演算子
    TK_AMPAMP,      // &&
    TK_PIPEPIPE,    // ||
    TK_BANG,        // !
    // 比較演算子
    TK_EQEQ,        // ==
    TK_NEQ,         // !=
    TK_LT,          // <
    TK_GT,          // >
    TK_LEQ,         // <=
    TK_GEQ,         // >=
    // 代入演算子
    TK_ASSIGN,          // =
    TK_PLUS_ASSIGN,     // +=
    TK_MINUS_ASSIGN,    // -=
    TK_STAR_ASSIGN,     // *=
    TK_SLASH_ASSIGN,    // /=
    TK_PCT_ASSIGN,      // %=
    TK_AMP_ASSIGN,      // &=
    TK_PIPE_ASSIGN,     // |=
    TK_CARET_ASSIGN,    // ^=
    TK_LSHIFT_ASSIGN,   // <<=
    TK_RSHIFT_ASSIGN,   // >>=
    // インクリメント・デクリメント
    TK_PLUSPLUS,    // ++
    TK_MINUSMINUS,  // --
    // 三項演算子
    TK_QUESTION,    // ?
    TK_COLON,       // :
    // 区切り文字
    TK_LPAREN,      // (
    TK_RPAREN,      // )
    TK_LBRACE,      // {
    TK_RBRACE,      // }
    TK_LBRACKET,    // [
    TK_RBRACKET,    // ]
    TK_SEMICOLON,   // ;
    TK_COMMA,       // ,
    TK_DOT,         // .
    // ファイル末尾
    TK_EOF,
} token_kind_t;

// トークン
typedef struct {
    token_kind_t kind;  // 種別
    std::string value;  // 文字列値
    int line;           // 行番号
} token_t;

// 字句解析してトークン列を生成する
void lex(const std::string &src, std::vector<token_t> &tokens);
