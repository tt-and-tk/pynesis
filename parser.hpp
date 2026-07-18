#pragma once
#include <string>
#include <vector>

#include "lexer.hpp"

// 基本型種別
typedef enum {
    BASE_CHAR, BASE_SHORT, BASE_INT, BASE_VOID, BASE_STRUCT,
} base_type_t;

// 型情報
typedef struct {
    base_type_t base = BASE_INT;  // 基本型
    bool is_signed = true;        // signed/unsigned (unsignedは未対応のため常にtrue)
    bool is_array = false;        // 配列かどうか
    int array_size = 0;           // 配列の要素数 (is_array==trueのとき有効)
    // 構造体名 (base==BASE_STRUCTのときのみ有効)
    // 構造体の配列宣言(struct Tag arr[N];のような，この構造体自体を複数並べる宣言)は非対応のため，
    // base==BASE_STRUCT かつ is_array==true の組み合わせは現れない
    std::string struct_name;
} type_t;

// ASTノード種別
typedef enum {
    // プログラム構造
    ND_PROGRAM,     // プログラム全体
    ND_FUNC_DEF,    // 関数定義
    ND_BLOCK,       // ブロック文 { ... }
    // 宣言
    ND_VAR_DECL,    // 変数宣言
    ND_STRUCT_DECL, // 構造体定義 (svalに構造体名，childrenにメンバ宣言(ND_VAR_DECL)を格納)
    // 制御構文
    ND_IF,          // if文
    ND_WHILE,       // while文
    ND_FOR,         // for文
    ND_DO_WHILE,    // do-while文
    ND_SWITCH,      // switch文
    ND_CASE,        // case節
    ND_DEFAULT,     // default節
    ND_BREAK,       // break文
    ND_CONTINUE,    // continue文
    ND_RETURN,      // return文
    // 式
    ND_CALL,        // 関数呼び出し
    ND_PRINT,       // 組み込み関数print (標準出力)
    ND_SCAN,        // 組み込み関数scan (標準入力)
    ND_ASSIGN,      // 代入式
    ND_BINOP,       // 二項演算
    ND_UNOP,        // 前置単項演算
    ND_POST_UNOP,   // 後置単項演算 (++/--)
    ND_TERNARY,     // 三項演算子
    ND_SIZEOF,      // sizeof式
    ND_INT_LIT,     // 整数リテラル
    ND_CHAR_LIT,    // 文字リテラル
    ND_STRING_LIT,  // 文字列リテラル (svalに引用符なしの文字列内容を格納)
    ND_VAR,             // 変数参照
    ND_ARRAY_ACCESS,    // 配列要素アクセス a[i] (children: [インデックス式]，構造体メンバ配列なら[インデックス式, ND_MEMBER_ACCESS])
    ND_MEMBER_ACCESS,   // 構造体メンバアクセス a.b (children: [構造体変数(ND_VAR)]，svalにメンバ名を格納)
} node_kind_t;

// シンボル情報 (実体はanalyzer.hppで定義．node_tはポインタで参照するため前方宣言する)
struct symbol_t;

// ASTノード
struct node_t {
    node_kind_t kind;               // ノード種別
    std::vector<node_t *> children; // 子ノード
    std::string sval;               // 文字列値 (識別子名・演算子文字列)
    long long ival;                 // 整数値 (リテラル)
    type_t type;                    // 型情報 (意味解析後に確定)
    int line;                       // 行番号 (エラー報告用)
    const symbol_t *sym = nullptr;  // 名前解決の結果 (ND_VAR等がどの宣言を指すか，意味解析後に確定)
};

// トークン列を受け取り，ASTを生成するパーサ
//
// パーサの責務は「トークンの並びが文法に合っているか」のチェックまで．
//   エラーにするもの: 期待したトークンが来ない (例: 閉じ括弧がない，式が来るべき場所に ; がある)
//   見逃すもの: 構文としては正しいが意味的に不正な式．後段の意味解析で検査する．
//     例: 代入の左辺が変数でない (1 + 2 = x)，未宣言の変数参照，未定義関数の呼び出し
class Parser {
public:
    explicit Parser(const std::vector<token_t> &tokens);
    node_t *operator()();   // 構文解析を実行してASTのルートを返す

private:
    const std::vector<token_t> &tokens_;  // トークン列
    int pos_;                             // 現在の読み取り位置
    int anon_struct_count_;                // 無名構造体に割り当てる連番

    // ヘルパー
    const token_t &peek_token() const;                     // 現在のトークンを覗き見る (消費しない)
    token_kind_t peek_kind_ahead(int offset) const;        // pos_+offset先のトークン種別を返す (範囲外ならTK_EOF扱い)
    bool token_kind_is(token_kind_t kind) const;           // 現在のトークンの種別が一致するか調べる (消費しない)
    token_t get_token();                                   // トークンを取得して進める (検証なし)
    token_t get_token(token_kind_t kind);                  // 指定種別のトークンを取得して進める，違えばエラー
    node_t *new_node(node_kind_t kind);                    // 現在のトークンの行番号でASTノードを生成する
    static bool is_type_start(token_kind_t kind);          // 型の先頭になりうるトークン種別かどうか返す
    static bool is_assign_op(token_kind_t kind);           // 代入演算子のトークン種別かどうか返す
    static std::string token_kind_name(token_kind_t kind); // トークン種別をエラーメッセージ用の文字列に変換する
    static long long parse_int_literal(const std::string &text);   // 整数リテラル文字列を数値に変換する
    static long long parse_char_literal(const std::string &text);  // 文字リテラル文字列を文字コードに変換する
    static std::string parse_string_literal(const std::string &text);  // 文字列リテラルの引用符を除去しエスケープを解釈する
    // signed/unsigned修飾子と型キーワード(int/char/short/struct，allow_voidならvoidも)を読み，型情報を返す
    // 関数戻り値型・パラメータ型・変数宣言型・構造体メンバ型のいずれからも共通で呼ばれる
    type_t parse_type(bool allow_void);

    // 構文解析メソッド (parse_で始まる)
    node_t *parse_program();    // プログラム全体
    node_t *parse_func_def();   // 関数定義
    node_t *parse_block();      // ブロック { ... }
    node_t *parse_stmt();       // 文
    node_t *parse_var_decl();   // 変数宣言
    // 構造体定義 struct [構造体名] { メンバ宣言... } [変数名] ;
    // 構造体名省略時は無名構造体となり，その場での変数宣言を必須とする
    // 戻り値: [0]=構造体定義(ND_STRUCT_DECL)．変数も宣言する場合は[1]に変数宣言(ND_VAR_DECL)を追加する
    std::vector<node_t *> parse_struct_decl();
    node_t *parse_struct_member();  // 構造体メンバ宣言 (型 名前 [配列サイズ] ;)
    node_t *parse_return();     // return文
    node_t *parse_if();         // if文 (else if / else を含む)
    node_t *parse_while();      // while文
    node_t *parse_for();        // for文
    node_t *parse_do_while();   // do-while文
    node_t *parse_switch();     // switch文
    node_t *parse_case();       // case節
    node_t *parse_default();    // default節
    node_t *parse_break();      // break文
    node_t *parse_continue();   // continue文
    node_t *parse_param();      // 関数パラメータ (型 名前)
    node_t *parse_print();      // 組み込み関数print(char配列)
    node_t *parse_scan();       // 組み込み関数scan(char配列)
    node_t *parse_sizeof();     // sizeof(型名 または 変数名)
    node_t *parse_expr_stmt();  // 式文 (式 ;)
    // 式解析メソッド群
    // 優先順位の低い演算子ほど浅い関数が担当し，下記の順に呼び出しが連鎖する．
    //   parse_expr → parse_assign → parse_ternary → parse_binary → parse_unary → parse_postfix → parse_primary
    // 各関数は「自分の担当演算子を含むかもしれない式」を解析する．
    // 担当演算子が見つかればノードを作って包み，なければ下位の解析結果をそのまま返す(パススルー)．
    // つまり各関数が返す木のルートは「担当演算子か，それより優先順位の高いもの」のいずれかであり，
    // 自分より優先順位の低い演算子がルートになることはない(それは呼び出し元の浅い関数が担当する)．
    node_t *parse_expr();       // 式 (エントリポイント)
    node_t *parse_assign();             // 代入式 x = 式, x += 式 等
    node_t *parse_ternary();            // 三項演算子 a ? b : c
    node_t *parse_binary(int min_prec); // 二項演算子を含む式 (優先順位min_prec以上を処理)
    node_t *parse_unary();              // 前置単項演算子を含む式
    node_t *parse_postfix();            // 後置演算子・メンバアクセス・関数呼び出しを含む式
    node_t *parse_primary();    // 基本式 (リテラル・変数参照・括弧式)
};
