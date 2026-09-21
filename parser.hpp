#pragma once
#include <memory>
#include <string>
#include <vector>

#include "lexer.hpp"

// 基本型種別
// BASE_FUNCは関数ポインタが指す先(関数)を表し，シグネチャはtype_tのfunc_sigが持つ
// BASE_NULLPTRは組み込み定数nullptrだけが持つ型で，どのポインタ型とも比較・代入できる
typedef enum {
    BASE_CHAR, BASE_SHORT, BASE_INT, BASE_VOID, BASE_STRUCT, BASE_FUNC, BASE_NULLPTR,
} base_type_t;

// 関数ポインタのシグネチャ (実体は型情報の後で定義する．型情報が自身を含むため前方宣言する)
struct func_sig_t;

// 型情報
typedef struct type_t {
    base_type_t base = BASE_INT;  // 基本型
    bool is_signed = true;        // signed(true)/unsigned(false)
    bool is_array = false;        // 配列かどうか
    int array_size = 0;           // 配列の要素数 (is_array==trueのとき有効)
    // 構造体名 (base==BASE_STRUCTのときのみ有効)
    // base==BASE_STRUCT かつ is_array==true は，変数宣言で構造体自体を複数並べた配列
    // (struct Tag arr[N];)を表す．一方，構造体のメンバはスカラーまたは固定長配列のみで
    // 構成し，メンバ自身がstruct型になること(ネスト構造体)自体が非対応のため，
    // メンバの型としてbase==BASE_STRUCTが現れることはそもそもない
    std::string struct_name;
    // const修飾されているかどうか (constを付けられない対象への付与は構文解析でエラーにする)
    bool is_const = false;
    // ポインタの段数 (`*`の個数．0ならポインタでない)
    // is_arrayと同時に立つ場合はポインタの配列(int *table[4])を表し，その要素の型は段数を保ったスカラーになる
    int pointer_depth = 0;
    // 関数ポインタのシグネチャ (関数ポインタでなければnullptr)
    // 同じ型情報を複製しても指す先を共有できるよう，また型が自身を含みうるため，共有ポインタで持つ
    std::shared_ptr<func_sig_t> func_sig;
} type_t;

// 関数ポインタのシグネチャ (戻り値型と引数の型の並び)
struct func_sig_t {
    type_t return_type;                // 戻り値の型
    std::vector<type_t> param_types;   // 引数の型 (宣言順)
};

// 値としてポインタかどうかを返す (ポインタの配列は，値としては配列であってポインタではない)
bool is_pointer_value(const type_t &type);
// 関数ポインタかどうかを返す
bool is_func_pointer(const type_t &type);
// ポインタが指す先の型を返す (段数を1つ減らす)
type_t pointee_type(const type_t &type);
// 配列を値として使う際の型(先頭要素へのポインタ)を返す．配列でなければそのままの型を返す
type_t decayed_type(const type_t &type);
// 型を，エラーメッセージに書く表記(`int *`等)に変換する
std::string type_to_string(const type_t &type);

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
    ND_CALL,        // 関数呼び出し (children: [呼び出し先の式, 引数...])
    ND_PRINT,       // 組み込み関数print (標準出力)
    ND_SCAN,        // 組み込み関数scan (標準入力)
    ND_STREQ,       // 組み込み関数streq (char配列2つの内容比較)
    ND_STRCOPY,     // 組み込み関数strcopy (char配列のコピー)
    ND_ASSIGN,      // 代入式
    ND_BINOP,       // 二項演算
    ND_UNOP,        // 前置単項演算
    ND_POST_UNOP,   // 後置単項演算 (++/--)
    ND_TERNARY,     // 三項演算子
    ND_SIZEOF,      // sizeof式
    ND_INT_LIT,     // 整数リテラル
    ND_CHAR_LIT,    // 文字リテラル
    ND_STRING_LIT,  // 文字列リテラル (svalに引用符なしの文字列内容を格納)
    ND_NULLPTR,     // 組み込み定数nullptr
    ND_VAR,             // 変数参照
    ND_FUNC_ADDR,       // 関数の番地 (svalに関数名．呼び出し以外の文脈で書かれた関数名を意味解析が置き換える)
    ND_ADDR,            // 番地の取得 &x (children: [番地を取る式])
    ND_DEREF,           // 間接参照 *p (children: [ポインタの式])
    // 配列要素アクセス a[i] (children: 配列変数・ポインタ変数ならsvalに名前を持ち[インデックス式]，
    // それ以外なら[インデックス式, 添字を付ける基底の式(ND_MEMBER_ACCESS・ND_DEREF・ND_ARRAY_ACCESS)])．
    // 要素が構造体の場合，要素a[i]は単独では値を持たず，ND_MEMBER_ACCESSの基底か&の対象としてのみ使われる
    ND_ARRAY_ACCESS,
    // 構造体メンバアクセス a.b (children: [構造体変数(ND_VAR)・要素が構造体の配列要素(ND_ARRAY_ACCESS)・
    // 構造体ポインタの間接参照(ND_DEREF)のいずれか]，svalにメンバ名)．p->bはND_DEREFを基底とする形に展開する
    ND_MEMBER_ACCESS,
} node_kind_t;

// シンボル情報 (実体はanalyzer.hppで定義．node_tはポインタで参照するため前方宣言する)
struct symbol_t;

// ASTノード
struct node_t {
    node_kind_t kind;               // ノード種別
    std::vector<node_t *> children; // 子ノード
    std::string sval;               // 文字列値 (識別子名・演算子文字列)
    // 整数値．ノード種別ごとに次の値を持つ (リテラル・sizeof・caseの値以外は意味解析が確定させる)
    //   リテラル・sizeof・case: その値
    //   番地が実行時に決まるメンバアクセス: 構造体先頭からのメンバのオフセット(ワード単位)
    //   配列要素アクセス: 要素1個のバイト数
    //   ポインタの加減算・差，ポインタへの+=/-=，++/--: 1増減するごとに動かすバイト数 (整数の++/--は1)
    long long ival;
    type_t type;                    // 型情報 (意味解析後に確定)
    loc_t loc;                      // ソース上の位置 (エラー報告用)
    const symbol_t *sym = nullptr;  // 名前解決の結果 (ND_VAR等がどの宣言を指すか，意味解析後に確定)
    // 配列を値として使う式か (意味解析後に確定)．立っている場合，typeは先頭要素へのポインタになっており，
    // コード生成は配列の中身を読む代わりに先頭の番地を値として求める
    bool is_decayed = false;
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
    node_t *new_node(node_kind_t kind);                    // 現在のトークンの位置でASTノードを生成する
    static bool is_type_start(token_kind_t kind);          // 型の先頭になりうるトークン種別かどうか返す
    int count_type_tokens() const;                         // 現在位置から始まる型が占めるトークン数を返す
    static bool is_assign_op(token_kind_t kind);           // 代入演算子のトークン種別かどうか返す
    static std::string token_kind_name(token_kind_t kind); // トークン種別をエラーメッセージ用の文字列に変換する
    static long long parse_int_literal(const token_t &token);      // 整数リテラルのトークンを数値に変換する
    static long long parse_char_literal(const std::string &text);  // 文字リテラル文字列を文字コードに変換する
    static std::string parse_string_literal(const std::string &text);  // 文字列リテラルの引用符を除去しエスケープを解釈する
    // const修飾子・signed/unsigned修飾子と型キーワード(int/char/short/struct，allow_voidならvoidも)に
    // 続けてポインタの`*`を読み，型情報を返す
    // 関数戻り値型・パラメータ型・変数宣言型・構造体メンバ型のいずれからも共通で呼ばれる
    type_t parse_type(bool allow_void);
    type_t parse_base_type(bool allow_void);   // parse_typeのうち，ポインタの*より前(修飾子と型キーワード)を読む
    // 関数ポインタの宣言子 (*名前)(引数型...) を読み，型に戻り値型と引数型を結びつけて変数名を返す
    // 呼び出し時点のtypeは戻り値型を表しており，読み終えたtypeは関数ポインタ型になる
    std::string parse_func_pointer_declarator(type_t &type);

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
    node_t *parse_streq();      // 組み込み関数streq(char配列, char配列)
    node_t *parse_strcopy();    // 組み込み関数strcopy(char配列, char配列)
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
    node_t *parse_postfix();            // 後置演算子・メンバアクセス・配列添字・関数呼び出しを含む式
    node_t *parse_member_name(node_t *base);   // .・->の直後のメンバ名を読み，baseを基底とするメンバアクセスを返す
    node_t *parse_primary();    // 基本式 (リテラル・nullptr・変数参照・括弧式)
};
