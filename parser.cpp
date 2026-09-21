#include <map>
#include <stdexcept>

#include "parser.hpp"

// 二項演算子の優先順位表 (数値が大きいほど強く結合する)
const std::map<token_kind_t, int> g_binop_prec = {
    {TK_PIPEPIPE, 1},                                  // ||
    {TK_AMPAMP,   2},                                  // &&
    {TK_PIPE,     3},                                  // |
    {TK_CARET,    4},                                  // ^
    {TK_AMP,      5},                                  // &
    {TK_EQEQ, 6}, {TK_NEQ, 6},                         // == !=
    {TK_LT, 7}, {TK_GT, 7}, {TK_LEQ, 7}, {TK_GEQ, 7},  // < > <= >=
    {TK_LSHIFT, 8}, {TK_RSHIFT, 8},                    // << >>
    {TK_PLUS, 9}, {TK_MINUS, 9},                       // + -
    {TK_STAR, 10}, {TK_SLASH, 10}, {TK_PERCENT, 10},   // * / %
};

// 値としてポインタかどうかを返す (ポインタの配列は，値としては配列であってポインタではない)
bool is_pointer_value(const type_t &type) {
    return type.pointer_depth > 0 && !type.is_array;
}

// 関数ポインタかどうかを返す
bool is_func_pointer(const type_t &type) {
    return type.base == BASE_FUNC && is_pointer_value(type);
}

// ポインタが指す先の型を返す (段数を1つ減らす)
type_t pointee_type(const type_t &type) {
    type_t pointee = type;
    pointee.pointer_depth--;
    return pointee;
}

// 配列を値として使う際の型(先頭要素へのポインタ)を返す．配列でなければそのままの型を返す
type_t decayed_type(const type_t &type) {
    // 配列でない場合 (値として使っても型は変わらない)
    if (!type.is_array) return type;
    type_t pointer = type;
    pointer.is_array = false;
    pointer.array_size = 0;
    pointer.pointer_depth++;
    return pointer;
}

// 型を，エラーメッセージに書く表記(`int *`等)に変換する
std::string type_to_string(const type_t &type) {
    // 関数ポインタの場合 (戻り値型と引数型の並びで表す．引数なしは(void)と書く)
    if (type.base == BASE_FUNC) {
        const std::vector<type_t> &params = type.func_sig->param_types;   // 引数の型の並び
        std::string text = type_to_string(type.func_sig->return_type) + " (*)(";
        if (params.empty()) text += "void";
        for (size_t i = 0; i < params.size(); i++) {
            if (i > 0) text += ", ";
            text += type_to_string(params[i]);
        }
        return text + ")";
    }
    std::string text;   // 組み立てる表記
    switch (type.base) {
        case BASE_CHAR:    text = "char";  break;
        case BASE_SHORT:   text = "short"; break;
        case BASE_INT:     text = "int";   break;
        case BASE_VOID:    text = "void";  break;
        case BASE_STRUCT:  text = "struct " + type.struct_name; break;
        case BASE_NULLPTR: return "nullptr";
        default:           text = "?";     break;
    }
    // 符号なしの整数型の場合
    if (!type.is_signed && type.base != BASE_STRUCT && type.base != BASE_VOID) {
        text = "unsigned " + text;
    }
    // ポインタの段数だけ*を付ける
    if (type.pointer_depth > 0) {
        text += " " + std::string(type.pointer_depth, '*');
    }
    // 配列の場合
    if (type.is_array) {
        text += "[" + std::to_string(type.array_size) + "]";
    }
    return text;
}

// コンストラクタ: トークン列を受け取り，読み取り位置を初期化する
Parser::Parser(const std::vector<token_t> &tokens)
    : tokens_(tokens), pos_(0), anon_struct_count_(0) {}

// 構文解析を実行してASTのルートを返す
node_t *Parser::operator()() {
    return this->parse_program();
}

// 現在のトークンを覗き見る (posを進めない)
const token_t &Parser::peek_token() const {
    return this->tokens_[this->pos_];
}

// pos_からoffset先のトークン種別を返す (範囲外ならTK_EOF扱いにして安全に返す)
token_kind_t Parser::peek_kind_ahead(int offset) const {
    const size_t idx = this->pos_ + offset;
    if (idx >= this->tokens_.size()) {
        return TK_EOF;
    }
    return this->tokens_[idx].kind;
}

// 現在のトークンの種別が一致するか調べる (消費しない)
bool Parser::token_kind_is(token_kind_t kind) const {
    return this->tokens_[this->pos_].kind == kind;
}

// トークンを取得して進める (検証なし)
token_t Parser::get_token() {
    return this->tokens_[this->pos_++];
}

// 指定種別のトークンを取得して進める，違えばエラーを投げる
token_t Parser::get_token(token_kind_t kind) {
    const token_t &actual = this->tokens_[this->pos_];
    if (actual.kind != kind) {
        // EOFはvalueが空文字列のため，EOFであることを明示した表示にする
        const std::string actual_name = (actual.kind == TK_EOF) ? "EOF" : "'" + actual.value + "'";
        throw std::string("compiler error: expected ") + Parser::token_kind_name(kind)
              + " but got " + actual_name
              + " at " + loc_to_string(actual.loc);
    }
    return this->tokens_[this->pos_++];
}

// 現在のトークンの位置でASTノードを生成する
node_t *Parser::new_node(node_kind_t kind) {
    node_t *node = new node_t;
    node->kind   = kind;
    node->loc   = this->peek_token().loc;
    node->ival   = 0;
    return node;
}

// 型の先頭になりうるトークン種別かどうか返す
// voidは関数の戻り値型のほか，戻り値のない関数ポインタの宣言(void (*f)(int))の先頭にもなる
bool Parser::is_type_start(token_kind_t kind) {
    return kind == TK_INT || kind == TK_CHAR || kind == TK_SHORT || kind == TK_VOID
        || kind == TK_SIGNED || kind == TK_UNSIGNED || kind == TK_STRUCT || kind == TK_CONST;
}

// 現在位置から始まる型(const・符号修飾子・型キーワードまたはstruct 構造体名・ポインタの*)が占めるトークン数を返す
// 宣言の種類(関数定義か変数宣言か)を，型を読み進める前に判定するために使う
int Parser::count_type_tokens() const {
    int offset = 0;   // 現在位置から数えたトークン数
    // const修飾子がある場合
    if (this->peek_kind_ahead(offset) == TK_CONST) offset++;
    const token_kind_t sign_kind = this->peek_kind_ahead(offset);   // 符号修飾子がありうる位置のトークン種別
    // 符号修飾子がある場合
    if (sign_kind == TK_SIGNED || sign_kind == TK_UNSIGNED) offset++;
    // 構造体型は構造体名の分も数える
    if (this->peek_kind_ahead(offset) == TK_STRUCT) offset++;
    offset++;   // 型キーワード(構造体型なら構造体名)
    // ポインタの*を数える
    while (this->peek_kind_ahead(offset) == TK_STAR) offset++;
    return offset;
}

// 代入演算子のトークン種別かどうか返す
bool Parser::is_assign_op(token_kind_t kind) {
    return kind == TK_ASSIGN
        || kind == TK_PLUS_ASSIGN  || kind == TK_MINUS_ASSIGN
        || kind == TK_STAR_ASSIGN  || kind == TK_SLASH_ASSIGN || kind == TK_PCT_ASSIGN
        || kind == TK_AMP_ASSIGN   || kind == TK_PIPE_ASSIGN  || kind == TK_CARET_ASSIGN
        || kind == TK_LSHIFT_ASSIGN || kind == TK_RSHIFT_ASSIGN;
}

// トークン種別をエラーメッセージ用の文字列に変換する
std::string Parser::token_kind_name(token_kind_t kind) {
    switch (kind) {
        case TK_SEMICOLON: return "';'";
        case TK_LPAREN:    return "'('";
        case TK_RPAREN:    return "')'";
        case TK_LBRACE:    return "'{'";
        case TK_RBRACE:    return "'}'";
        case TK_VOID:      return "'void'";
        case TK_STAR:      return "'*'";
        case TK_IDENT:     return "identifier";
        case TK_EOF:       return "EOF";
        default:           return "token";
    }
}

// const修飾子・signed/unsigned修飾子と型キーワードに続けてポインタの*を読み，型情報を返す
// 関数戻り値型・パラメータ型・変数宣言型・構造体メンバ型のいずれからも共通で呼ばれる
// allow_voidがtrueのときのみvoid型を許可する(関数の戻り値型と，関数ポインタの戻り値型になりうる宣言の先頭で許可する)
// constはどの文脈でも読んで型情報に記録し，許可するかどうかは呼び出し元が文脈に応じて判定する
type_t Parser::parse_type(bool allow_void) {
    const loc_t loc = this->peek_token().loc;   // 型の先頭の位置 (エラー報告用)
    type_t type = this->parse_base_type(allow_void);

    // ポインタの*を数える
    while (this->token_kind_is(TK_STAR)) {
        this->get_token();
        type.pointer_depth++;
    }
    // voidへのポインタの場合 (指す先の大きさが決まらず，読み書きも加減算もできないため非対応)
    if (type.base == BASE_VOID && type.pointer_depth > 0) {
        throw std::string("compiler error: void pointer is not supported at ") + loc_to_string(loc);
    }
    // constとポインタを組み合わせた場合 (constは値をコンパイル時に埋め込む整数の定数にのみ使うため)
    if (type.is_const && type.pointer_depth > 0) {
        throw std::string("compiler error: pointer cannot be const at ") + loc_to_string(loc);
    }
    return type;
}

// const修飾子・signed/unsigned修飾子と型キーワードを読み，ポインタの*を含まない型情報を返す
type_t Parser::parse_base_type(bool allow_void) {
    // const修飾子 (型名より前にのみ書ける)
    const bool is_const = this->token_kind_is(TK_CONST);   // const修飾子が付いているか
    // const修飾子が付いている場合は読み進める
    if (is_const) {
        this->get_token();
    }

    // signed/unsigned修飾子 (省略時はsigned．整数型キーワードの直前にのみ書ける)
    const bool has_sign = this->token_kind_is(TK_SIGNED) || this->token_kind_is(TK_UNSIGNED);   // 符号修飾子が付いているか
    const bool is_signed = !this->token_kind_is(TK_UNSIGNED);                                    // 符号なしでないか
    // 符号修飾子が付いている場合は読み進め，直後が整数型キーワードであることを確認する
    if (has_sign) {
        const token_t sign = this->get_token();   // 符号修飾子
        if (!this->token_kind_is(TK_INT) && !this->token_kind_is(TK_CHAR) && !this->token_kind_is(TK_SHORT)) {
            throw std::string("compiler error: expected 'int', 'char' or 'short' after '") + sign.value
                  + "' at " + loc_to_string(sign.loc);
        }
    }

    type_t type;
    type.is_signed = is_signed;
    type.is_const = is_const;

    // 構造体型: struct 構造体名
    if (this->token_kind_is(TK_STRUCT)) {
        this->get_token();
        type.base = BASE_STRUCT;
        type.struct_name = this->get_token(TK_IDENT).value;
        return type;
    }

    // void型 (関数の戻り値型でのみ許可する)
    if (allow_void && this->token_kind_is(TK_VOID)) {
        type.base = BASE_VOID;
        this->get_token();
        return type;
    }

    // スカラー型キーワード
    if (this->token_kind_is(TK_INT))   { type.base = BASE_INT;   this->get_token(); return type; }
    if (this->token_kind_is(TK_CHAR))  { type.base = BASE_CHAR;  this->get_token(); return type; }
    if (this->token_kind_is(TK_SHORT)) { type.base = BASE_SHORT; this->get_token(); return type; }

    throw std::string("compiler error: expected type at ")
          + loc_to_string(this->peek_token().loc);
}

// プログラム全体を解析してND_PROGRAMを返す
// 単なるトークン列を木構造に起こして返す
node_t *Parser::parse_program() {
    node_t *node = this->new_node(ND_PROGRAM);    // 木構造のルート

    //
    // メンバ変数にトークン列を持つ
    // これを前から順番に呼んでいきながら，
    // 木構造に起こして上記node変数に格納していく
    //

    // ファイル終端まで繰り返す
    while (!this->token_kind_is(TK_EOF)) {
        // 構造体名の直後(無名構造体ならstructキーワードの直後)が'{'なら構造体定義
        // (名前のみ／名前+即座の変数宣言／無名構造体+即座の変数宣言のいずれも，parse_struct_declが一括して構文解析する)
        const bool has_tag = this->peek_kind_ahead(1) == TK_IDENT;   // struct の直後に構造体名があるか
        if (this->token_kind_is(TK_STRUCT) && this->peek_kind_ahead(has_tag ? 2 : 1) == TK_LBRACE) {
            for (node_t *decl : this->parse_struct_decl()) {
                node->children.push_back(decl);
            }
        }
        // 型で始まるなら関数定義またはグローバル変数宣言
        else if (Parser::is_type_start(this->peek_token().kind)) {
            const int offset = this->count_type_tokens();   // 型の直後のトークンまでの数
            // 型の直後が識別子でも'('(関数ポインタの宣言子)でもなければエラー
            if (this->peek_kind_ahead(offset) != TK_IDENT && this->peek_kind_ahead(offset) != TK_LPAREN) {
                throw std::string("compiler error: expected identifier after type at ")
                      + loc_to_string(this->peek_token().loc);
            }
            // 型 識別子 '(' なら関数定義，それ以外(関数ポインタの宣言子を含む)は変数宣言
            if (this->peek_kind_ahead(offset) == TK_IDENT && this->peek_kind_ahead(offset + 1) == TK_LPAREN) {
                node->children.push_back(this->parse_func_def());
            } else {
                node->children.push_back(this->parse_var_decl());
            }
        }
        // それ以外がファイル直下にあるならエラー
        else {
            throw std::string("compiler error: expected function or variable declaration at ")
                  + loc_to_string(this->peek_token().loc);
        }
    }

    return node;
}

// 関数定義を解析してND_FUNC_DEFを返す
// 構文: 戻り値型 関数名(void) ブロック
node_t *Parser::parse_func_def() {
    node_t *node = this->new_node(ND_FUNC_DEF);   // 関数ノード

    // 戻り値型を読む (void・整数型・ポインタを許可し，構造体そのもの・constは非対応)
    node->type = this->parse_type(true);
    if (node->type.base == BASE_STRUCT && node->type.pointer_depth == 0) {
        throw std::string("compiler error: struct cannot be used as a function return type at ")
              + loc_to_string(node->loc);
    }
    // 戻り値型にconstが付いている場合
    if (node->type.is_const) {
        throw std::string("compiler error: function return type cannot be const at ")
              + loc_to_string(node->loc);
    }

    // 関数名
    node->sval = this->get_token(TK_IDENT).value; // 関数名
    this->get_token(TK_LPAREN);                   // 開きカッコ

    // パラメータリスト: (void) / () は引数なし，それ以外は型+名前のカンマ区切り
    if (this->token_kind_is(TK_VOID) && this->peek_kind_ahead(1) == TK_RPAREN) {
        // (void) : 引数なし
        this->get_token();
    } else if (!this->token_kind_is(TK_RPAREN)) {
        // パラメータを1つ以上パースする
        node->children.push_back(this->parse_param());
        while (!this->token_kind_is(TK_RPAREN)) {
            this->get_token(TK_COMMA);    // , を消費
            node->children.push_back(this->parse_param());
        }
    }
    // () : 引数なしの場合はそのまま閉じ括弧へ

    this->get_token(TK_RPAREN);                   // 閉じ括弧

    // 関数の中身を追加する
    node->children.push_back(this->parse_block());

    return node;
}

// ブロックを解析してND_BLOCKを返す
// 構文: { 文... }
node_t *Parser::parse_block() {
    node_t *node = this->new_node(ND_BLOCK);    // ブロックのルート

    // 開き波括弧
    this->get_token(TK_LBRACE);

    // } が来るまで文を繰り返し読む
    while (!this->token_kind_is(TK_RBRACE) && !this->token_kind_is(TK_EOF)) {
        node->children.push_back(this->parse_stmt());
    }

    // 閉じ波括弧
    this->get_token(TK_RBRACE);

    return node;
}

// 文を解析してASTノードを返す
node_t *Parser::parse_stmt() {
    // 型キーワードで始まれば変数宣言
    if (Parser::is_type_start(this->peek_token().kind)) {
        // structの場合のみ，先に構造体定義の禁止チェックを行う
        if (this->token_kind_is(TK_STRUCT)) {
            const bool has_tag = this->peek_kind_ahead(1) == TK_IDENT;
            const token_kind_t next_kind = has_tag ? this->peek_kind_ahead(2) : this->peek_kind_ahead(1);
            // struct [構造体名] { ... : 構造体定義はグローバル直下でのみ許可する
            if (next_kind == TK_LBRACE) {
                throw std::string("compiler error: struct definition is only allowed at global scope at ")
                      + loc_to_string(this->peek_token().loc);
            }
            // ここでエラーにならなければ，struct 構造体名 変数名; (既存の構造体定義を使った変数宣言)であり，
            // 通常の変数宣言と同じくparse_var_declに処理を委ねてよい
        }
        return this->parse_var_decl();
    }
    // ブロック { ... }
    if (this->token_kind_is(TK_LBRACE)) {
        return this->parse_block();
    }
    // return文
    if (this->token_kind_is(TK_RETURN)) {
        return this->parse_return();
    }
    // if文
    if (this->token_kind_is(TK_IF)) {
        return this->parse_if();
    }
    // while文
    if (this->token_kind_is(TK_WHILE)) {
        return this->parse_while();
    }
    // for文
    if (this->token_kind_is(TK_FOR)) {
        return this->parse_for();
    }
    // do-while文
    if (this->token_kind_is(TK_DO)) {
        return this->parse_do_while();
    }
    // switch文
    if (this->token_kind_is(TK_SWITCH)) {
        return this->parse_switch();
    }
    // break文
    if (this->token_kind_is(TK_BREAK)) {
        return this->parse_break();
    }
    // continue文
    if (this->token_kind_is(TK_CONTINUE)) {
        return this->parse_continue();
    }
    // 空文 ; (何もしない文．空ブロックとして表現する)
    if (this->token_kind_is(TK_SEMICOLON)) {
        node_t *node = this->new_node(ND_BLOCK);
        this->get_token(TK_SEMICOLON);
        return node;
    }
    // それ以外は式文 (式 ;)
    return this->parse_expr_stmt();
}

// return文を解析してND_RETURNを返す
// 構文: return ; | return 式 ;
node_t *Parser::parse_return() {
    node_t *node = this->new_node(ND_RETURN);
    this->get_token(TK_RETURN);
    // セミコロンでなければ戻り値の式をパースする
    if (!this->token_kind_is(TK_SEMICOLON)) {
        node->children.push_back(this->parse_expr());
    }
    this->get_token(TK_SEMICOLON);
    return node;
}

// 式文を解析する
// 構文: 式 ;
node_t *Parser::parse_expr_stmt() {
    node_t *node = this->parse_expr();
    this->get_token(TK_SEMICOLON);
    return node;
}

// if文を解析してND_IFを返す
// 構文: if ( 条件 ) 文 [else 文]   (本体は単文でもブロックでも可)
// 子ノードは [条件, then節] または [条件, then節, else節]
node_t *Parser::parse_if() {
    node_t *node = this->new_node(ND_IF);
    this->get_token(TK_IF);
    this->get_token(TK_LPAREN);
    node->children.push_back(this->parse_expr());    // 条件
    this->get_token(TK_RPAREN);
    node->children.push_back(this->parse_stmt());    // then節

    // elseがあれば読む (else直後にifが来ればelse ifの連鎖になる)
    if (this->token_kind_is(TK_ELSE)) {
        this->get_token(TK_ELSE);
        node->children.push_back(this->parse_stmt());  // else節
    }

    return node;
}

// while文を解析してND_WHILEを返す
// 構文: while ( 条件 ) 文   (本体は単文でもブロックでも可)
// 子ノードは [条件, 本体]
node_t *Parser::parse_while() {
    node_t *node = this->new_node(ND_WHILE);
    this->get_token(TK_WHILE);
    this->get_token(TK_LPAREN);
    node->children.push_back(this->parse_expr());    // 条件
    this->get_token(TK_RPAREN);
    node->children.push_back(this->parse_stmt());    // 本体
    return node;
}

// for文を解析してND_FORを返す
// 構文: for ( 初期化; 条件; 更新 ) 文   (各部は省略可能，本体は単文でもブロックでも可)
// 子ノードは [初期化, 条件, 更新, 本体]．省略された部分はnullptrを入れて常に4子に固定する
node_t *Parser::parse_for() {
    node_t *node = this->new_node(ND_FOR);
    this->get_token(TK_FOR);
    this->get_token(TK_LPAREN);

    // 初期化部: 型で始まれば変数宣言(；まで消費)，空なら nullptr，それ以外は式
    if (this->token_kind_is(TK_SEMICOLON)) {
        node->children.push_back(nullptr);
        this->get_token(TK_SEMICOLON);
    } else if (Parser::is_type_start(this->peek_token().kind)) {
        node->children.push_back(this->parse_var_decl());   // 末尾の ; まで消費する
    } else {
        node->children.push_back(this->parse_expr());
        this->get_token(TK_SEMICOLON);
    }

    // 条件部: 空なら nullptr
    if (this->token_kind_is(TK_SEMICOLON)) {
        node->children.push_back(nullptr);
    } else {
        node->children.push_back(this->parse_expr());
    }
    this->get_token(TK_SEMICOLON);

    // 更新部: 空なら nullptr
    if (this->token_kind_is(TK_RPAREN)) {
        node->children.push_back(nullptr);
    } else {
        node->children.push_back(this->parse_expr());
    }
    this->get_token(TK_RPAREN);

    // 本体
    node->children.push_back(this->parse_stmt());
    return node;
}

// do-while文を解析してND_DO_WHILEを返す
// 構文: do 文 while ( 条件 ) ;   (本体は単文でもブロックでも可)
// 子ノードは [本体, 条件]
node_t *Parser::parse_do_while() {
    node_t *node = this->new_node(ND_DO_WHILE);
    this->get_token(TK_DO);
    node->children.push_back(this->parse_stmt());    // 本体
    this->get_token(TK_WHILE);
    this->get_token(TK_LPAREN);
    node->children.push_back(this->parse_expr());    // 条件
    this->get_token(TK_RPAREN);
    this->get_token(TK_SEMICOLON);
    return node;
}

// switch文を解析してND_SWITCHを返す
// 構文: switch ( 条件 ) { case節・default節・文を並べる }
// 子ノードは [条件式, 本体の文とcase/defaultラベルを平坦に並べたもの]
// case/defaultはラベルとして文の列に混ざる(フォールスルーをそのまま表現するため)
node_t *Parser::parse_switch() {
    node_t *node = this->new_node(ND_SWITCH);
    this->get_token(TK_SWITCH);
    this->get_token(TK_LPAREN);
    node->children.push_back(this->parse_expr());    // 条件式
    this->get_token(TK_RPAREN);
    this->get_token(TK_LBRACE);

    // } までcase節・default節・文を平坦に読む
    while (!this->token_kind_is(TK_RBRACE) && !this->token_kind_is(TK_EOF)) {
        if (this->token_kind_is(TK_CASE)) {
            node->children.push_back(this->parse_case());
        } else if (this->token_kind_is(TK_DEFAULT)) {
            node->children.push_back(this->parse_default());
        } else {
            node->children.push_back(this->parse_stmt());
        }
    }

    this->get_token(TK_RBRACE);
    return node;
}

// case節を解析してND_CASEを返す
// 構文: case 定数式 :   (値は子ノードの式．意味解析で定数畳み込みする)
node_t *Parser::parse_case() {
    node_t *node = this->new_node(ND_CASE);
    this->get_token(TK_CASE);
    node->children.push_back(this->parse_expr());    // caseの値 (定数式)
    this->get_token(TK_COLON);
    return node;
}

// default節を解析してND_DEFAULTを返す
// 構文: default :
node_t *Parser::parse_default() {
    node_t *node = this->new_node(ND_DEFAULT);
    this->get_token(TK_DEFAULT);
    this->get_token(TK_COLON);
    return node;
}

// break文を解析してND_BREAKを返す
// 構文: break ;
node_t *Parser::parse_break() {
    node_t *node = this->new_node(ND_BREAK);
    this->get_token(TK_BREAK);
    this->get_token(TK_SEMICOLON);
    return node;
}

// continue文を解析してND_CONTINUEを返す
// 構文: continue ;
node_t *Parser::parse_continue() {
    node_t *node = this->new_node(ND_CONTINUE);
    this->get_token(TK_CONTINUE);
    this->get_token(TK_SEMICOLON);
    return node;
}

// 組み込み関数printを解析してND_PRINTを返す
// 構文: print ( char配列 )  ※ ヌル終端までを標準出力へ出力する
node_t *Parser::parse_print() {
    node_t *node = this->new_node(ND_PRINT);
    this->get_token(TK_PRINT);                       // print
    this->get_token(TK_LPAREN);                      // (
    node->children.push_back(this->parse_expr());    // 出力する配列
    this->get_token(TK_RPAREN);                      // )
    return node;
}

// 組み込み関数scanを解析してND_SCANを返す
// 構文: scan ( char配列 )  ※ 改行までの1行をヌル終端付きで配列へ格納する
// 対象は識別子1個(TK_IDENT)の配列変数のみで，構造体のメンバ配列(単一の構造体変数のものも含む)へは
// 未対応．メンバ配列はコンパイル時に確定したアドレスに配置されるため技術的には書き込み先にできるが，
// printやstreq/strcopyのように任意の式(parse_expr)を受け付ける構文にはまだ拡張していない(将来対応予定)
node_t *Parser::parse_scan() {
    node_t *node = this->new_node(ND_SCAN);
    this->get_token(TK_SCAN);                         // scan
    this->get_token(TK_LPAREN);                       // (
    node_t *var = this->new_node(ND_VAR);             // 格納先配列
    var->sval = this->get_token(TK_IDENT).value;
    node->children.push_back(var);
    this->get_token(TK_RPAREN);                       // )
    return node;
}

// 組み込み関数streqを解析してND_STREQを返す
// 構文: streq ( char配列 , char配列 )  ※ 両方の内容が一致するか比較する
node_t *Parser::parse_streq() {
    node_t *node = this->new_node(ND_STREQ);
    this->get_token(TK_STREQ);                       // streq
    this->get_token(TK_LPAREN);                      // (
    node->children.push_back(this->parse_expr());    // 比較対象1
    this->get_token(TK_COMMA);                       // ,
    node->children.push_back(this->parse_expr());    // 比較対象2
    this->get_token(TK_RPAREN);                      // )
    return node;
}

// 組み込み関数strcopyを解析してND_STRCOPYを返す
// 構文: strcopy ( char配列 , char配列 )  ※ 第2引数の内容を第1引数へヌル終端付きでコピーする
node_t *Parser::parse_strcopy() {
    node_t *node = this->new_node(ND_STRCOPY);
    this->get_token(TK_STRCOPY);                     // strcopy
    this->get_token(TK_LPAREN);                      // (
    node->children.push_back(this->parse_expr());    // コピー先
    this->get_token(TK_COMMA);                       // ,
    node->children.push_back(this->parse_expr());    // コピー元
    this->get_token(TK_RPAREN);                      // )
    return node;
}

// sizeof式を解析してND_SIZEOFを返す
// 構文: sizeof ( 型名 ) または sizeof ( 変数名 )  TODO: 任意の式には非対応
// 型名は整数型(符号修飾子付きを含む)のみ．型名の場合はnode->typeに型を格納し(children空)，
// 変数名の場合はchildren[0]にND_VARを格納する(意味解析で解決)
node_t *Parser::parse_sizeof() {
    node_t *node = this->new_node(ND_SIZEOF);
    this->get_token(TK_SIZEOF);                      // sizeof
    this->get_token(TK_LPAREN);                      // (

    const token_kind_t kind = this->peek_token().kind;
    // 整数型キーワード(符号修飾子付きを含む)なら型名として読む
    if (kind == TK_INT || kind == TK_CHAR || kind == TK_SHORT || kind == TK_SIGNED || kind == TK_UNSIGNED) {
        node->type = this->parse_type(false);
    } else {
        // 型名でなければ変数名として解析する (意味解析で型を確定する)
        node_t *var = this->new_node(ND_VAR);
        var->sval = this->get_token(TK_IDENT).value;
        node->children.push_back(var);
    }

    this->get_token(TK_RPAREN);                      // )
    return node;
}

// 関数パラメータを解析してND_VAR_DECLを返す
// 構文: 型 変数名，または関数ポインタの宣言子 (初期化子・セミコロンなし)
node_t *Parser::parse_param() {
    node_t *node = this->new_node(ND_VAR_DECL);

    // パラメータの型 (voidは関数ポインタの戻り値型としてのみ書ける)
    node->type = this->parse_type(true);
    // 構造体そのものの場合 (値をコピーする仕組みが無いため．構造体へのポインタは渡せる)
    if (node->type.base == BASE_STRUCT && node->type.pointer_depth == 0) {
        throw std::string("compiler error: struct cannot be used as a function parameter type at ")
              + loc_to_string(node->loc);
    }
    // パラメータの型にconstが付いている場合
    if (node->type.is_const) {
        throw std::string("compiler error: function parameter cannot be const at ")
              + loc_to_string(node->loc);
    }

    // 関数ポインタのパラメータなら宣言子から名前を，そうでなければ名前を直接読む
    if (this->token_kind_is(TK_LPAREN)) {
        node->sval = this->parse_func_pointer_declarator(node->type, true);
    } else {
        node->sval = this->get_token(TK_IDENT).value;
    }
    // void型のパラメータの場合 (値を持たない型のため，関数ポインタの戻り値型以外には使えない)
    if (node->type.base == BASE_VOID) {
        throw std::string("compiler error: parameter cannot be void at ") + loc_to_string(node->loc);
    }
    // 名前の後に[]がある場合 (配列を受け取る書き方は，配列に見えて実体がポインタで紛らわしいため受け付けない)
    if (this->token_kind_is(TK_LBRACKET)) {
        type_t pointer = node->type;   // 同じ要素型へのポインタ (書き換え方の案内に使う)
        pointer.pointer_depth++;
        throw std::string("compiler error: array parameter is not supported; declare it as a pointer '")
              + type_to_string(pointer) + node->sval + "' at " + loc_to_string(node->loc);
    }

    return node;
}

// 関数ポインタの宣言子 (*名前)(引数型...) を読み，型に戻り値型と引数型を結びつけて変数名を返す
// 引数は型だけを書いても，型に続けて名前を書いてもよい(名前は読み捨てる)．(void)・()は引数なし
// 引数の型にも関数ポインタの宣言子を書け，その宣言子では名前を省ける(int (*)(int))．
// 名前を省いた宣言子は空の名前を返す．名前で参照する宣言ではname_requiredをtrueにする
std::string Parser::parse_func_pointer_declarator(type_t &type, bool name_required) {
    const loc_t loc = this->peek_token().loc;   // 宣言子の先頭の位置 (エラー報告用)
    // 関数ポインタを返す関数ポインタ・関数ポインタへのポインタは読まない
    // (戻り値型に*が付く場合はポインタを返す関数へのポインタとして扱う．宣言子の中の*は1個に限る)
    this->get_token(TK_LPAREN);
    this->get_token(TK_STAR);
    // 名前が必要な宣言か，名前が書かれている場合は名前を読む
    std::string name;   // 変数名 (省いた場合は空)
    if (name_required || this->token_kind_is(TK_IDENT)) {
        name = this->get_token(TK_IDENT).value;
    }
    this->get_token(TK_RPAREN);

    // 読み終えるまでの型は戻り値型を表している
    auto sig = std::make_shared<func_sig_t>();   // 関数ポインタのシグネチャ
    sig->return_type = type;
    // 構造体そのものを返す関数へのポインタの場合 (関数の戻り値に構造体を使えないのと同じ理由)
    if (sig->return_type.base == BASE_STRUCT && sig->return_type.pointer_depth == 0) {
        throw std::string("compiler error: struct cannot be used as a function return type at ")
              + loc_to_string(loc);
    }
    // 戻り値型にconstが付いている場合 (関数の戻り値型にconstを使えないのと同じ理由)
    if (sig->return_type.is_const) {
        throw std::string("compiler error: function return type cannot be const at ")
              + loc_to_string(loc);
    }

    // 引数の型の並びを読む
    this->get_token(TK_LPAREN);
    // (void) : 引数なし
    if (this->token_kind_is(TK_VOID) && this->peek_kind_ahead(1) == TK_RPAREN) {
        this->get_token();
    }
    // () 以外 : 引数の型をカンマ区切りで読む
    else if (!this->token_kind_is(TK_RPAREN)) {
        while (true) {
            const loc_t param_loc = this->peek_token().loc;   // 引数の型の位置 (エラー報告用)
            type_t param_type = this->parse_type(true);       // 引数の型 (voidは，引数を関数ポインタにする場合の戻り値型としてのみ書ける)
            // 関数ポインタの引数の場合 (宣言子の名前は読み捨てる)
            if (this->token_kind_is(TK_LPAREN)) {
                this->parse_func_pointer_declarator(param_type, false);
            }
            // void型の引数の場合 (値を持たない型のため)
            if (param_type.base == BASE_VOID) {
                throw std::string("compiler error: parameter cannot be void at ") + loc_to_string(param_loc);
            }
            // 構造体そのものを受け取る場合 (関数の引数に構造体を使えないのと同じ理由)
            if (param_type.base == BASE_STRUCT && param_type.pointer_depth == 0) {
                throw std::string("compiler error: struct cannot be used as a function parameter type at ")
                      + loc_to_string(param_loc);
            }
            // constの引数の場合 (関数の引数にconstを使えないのと同じ理由)
            if (param_type.is_const) {
                throw std::string("compiler error: function parameter cannot be const at ")
                      + loc_to_string(param_loc);
            }
            sig->param_types.push_back(param_type);
            // 引数名があれば読み捨てる
            if (this->token_kind_is(TK_IDENT)) this->get_token();
            if (!this->token_kind_is(TK_COMMA)) break;
            this->get_token(TK_COMMA);
        }
    }
    this->get_token(TK_RPAREN);

    // 型を関数ポインタにする (戻り値型はシグネチャへ移した)
    type = type_t{};
    type.base = BASE_FUNC;
    type.pointer_depth = 1;
    type.func_sig = sig;
    return name;
}

// 変数宣言を解析してND_VAR_DECLを返す
// 構文: [const] [signed|unsigned] 型 [*...] 変数名 [= 式] ;
//       戻り値型 (*変数名)(引数型...) [= 式] ;   (関数ポインタ)
// const変数はスカラー型のみで，初期化子を必須とする．
// 構造体型の場合は次の2形式のみ許可する(初期化子は非対応)．構造体へのポインタは通常のスカラー変数と同じ扱い．
//   struct 構造体名 変数名;         (単一変数)
//   struct 構造体名 変数名[サイズ]; (配列，サイズは省略不可)
node_t *Parser::parse_var_decl() {
    node_t *node = this->new_node(ND_VAR_DECL);    // 変数宣言部

    // 型を読む (voidは関数ポインタの戻り値型としてのみ書ける)
    node->type = this->parse_type(true);

    // 関数ポインタなら宣言子から変数名を，そうでなければ変数名を直接読む
    const bool is_func_pointer_decl = this->token_kind_is(TK_LPAREN);   // 関数ポインタの宣言子か
    if (is_func_pointer_decl) {
        node->sval = this->parse_func_pointer_declarator(node->type, true);
    } else {
        node->sval = this->get_token(TK_IDENT).value;
    }
    // void型の変数の場合 (値を持たない型のため，関数ポインタの戻り値型以外には使えない)
    if (node->type.base == BASE_VOID) {
        throw std::string("compiler error: variable cannot be void at ") + loc_to_string(node->loc);
    }
    // 関数ポインタの配列の場合 (宣言子の書き方を複雑にするため非対応)
    if (is_func_pointer_decl && this->token_kind_is(TK_LBRACKET)) {
        throw std::string("compiler error: array of function pointers is not supported at ")
              + loc_to_string(node->loc);
    }

    // const変数: 配列・構造体は初期値リストで値を与える手段がないため非対応とし，
    // スカラーは値を与えないと使い道がないため初期化子を必須とする
    // (ポインタのconstはparse_typeで，関数ポインタの戻り値型のconstは宣言子の解析でエラーにしている)
    if (node->type.is_const) {
        // 構造体変数の場合
        if (node->type.base == BASE_STRUCT) {
            throw std::string("compiler error: struct variable cannot be const at ")
                  + loc_to_string(node->loc);
        }
        // 配列の場合
        if (this->token_kind_is(TK_LBRACKET)) {
            throw std::string("compiler error: array cannot be const at ")
                  + loc_to_string(node->loc);
        }
        // 初期化子がない場合
        if (!this->token_kind_is(TK_ASSIGN)) {
            throw std::string("compiler error: const variable '") + node->sval
                  + "' requires an initializer at " + loc_to_string(node->loc);
        }
    }

    // 構造体変数: 配列にも対応する(要素数省略・初期化子はメンバ初期化の仕組みが無いため非対応)
    if (node->type.base == BASE_STRUCT && node->type.pointer_depth == 0) {
        // 変数名の後に[があれば配列宣言 (無ければ単一変数)
        if (this->token_kind_is(TK_LBRACKET)) {
            this->get_token(TK_LBRACKET);
            node->type.is_array = true;
            node->children.push_back(this->parse_expr());   // 配列サイズ (定数式，意味解析で畳み込む)
            this->get_token(TK_RBRACKET);
        } else if (this->token_kind_is(TK_ASSIGN)) {
            throw std::string("compiler error: struct variable initializer is not supported at ")
                  + loc_to_string(this->peek_token().loc);
        }
        this->get_token(TK_SEMICOLON);
        return node;
    }

    // 配列宣言: 変数名の後に [ サイズ ] または [] があれば配列
    if (this->token_kind_is(TK_LBRACKET)) {
        this->get_token(TK_LBRACKET);           // [
        node->type.is_array = true;
        if (this->token_kind_is(TK_RBRACKET)) {
            // サイズ省略: char msg[] = "hello"; の形式 (サイズは意味解析で文字列長から決定する)
            this->get_token(TK_RBRACKET);       // ]
            this->get_token(TK_ASSIGN);         // =
            node->children.push_back(this->parse_expr());  // 文字列リテラル (ND_STRING_LIT)
        } else {
            // サイズ明示: int table[10]; の形式
            node->children.push_back(this->parse_expr());
            this->get_token(TK_RBRACKET);       // ]
        }
    }
    // スカラー変数の初期化式があれば読む
    else if (this->token_kind_is(TK_ASSIGN)) {
        this->get_token();
        node->children.push_back(this->parse_expr());
    }

    // 文末のセミコロン
    this->get_token(TK_SEMICOLON);

    return node;
}

// 構造体メンバを1つ解析してND_VAR_DECLを返す (構造体自体の配列ではなく，メンバ自身が配列になりうるだけ)
// 構文: 型 [*...] 名前 [ 要素数 ] ;，または関数ポインタの宣言子 ;   (初期化子・ネスト構造体メンバは非対応)
// parse_struct_declのループから，メンバの数だけ繰り返し呼ばれる
node_t *Parser::parse_struct_member() {
    node_t *node = this->new_node(ND_VAR_DECL);

    // メンバの型を読む (スカラー・ポインタまたは固定長配列のみ許容，ネスト構造体は非対応)．
    // この時点で構造体そのものかどうかだけ判定するため，構造体をメンバとして持つメンバ宣言
    // (struct Outer { struct Inner arr[3]; }のarr．配列の要素数を読むより前の判定)も同じエラーで弾かれる．
    // 構造体へのポインタは1ワードの番地であり，自身の構造体を指すメンバ(struct Node *next)も書ける
    node->type = this->parse_type(true);
    if (node->type.base == BASE_STRUCT && node->type.pointer_depth == 0) {
        throw std::string("compiler error: nested struct members are not supported at ")
              + loc_to_string(node->loc);
    }
    // メンバの型にconstが付いている場合 (メンバの初期化子が非対応で値を与える手段がないため，constメンバも非対応)
    if (node->type.is_const) {
        throw std::string("compiler error: struct member cannot be const at ")
              + loc_to_string(node->loc);
    }

    // 関数ポインタのメンバなら宣言子からメンバ名を，そうでなければメンバ名を直接読む
    const bool is_func_pointer_decl = this->token_kind_is(TK_LPAREN);   // 関数ポインタの宣言子か
    if (is_func_pointer_decl) {
        node->sval = this->parse_func_pointer_declarator(node->type, true);
    } else {
        node->sval = this->get_token(TK_IDENT).value;
    }
    // void型のメンバの場合 (値を持たない型のため，関数ポインタの戻り値型以外には使えない)
    if (node->type.base == BASE_VOID) {
        throw std::string("compiler error: struct member cannot be void at ") + loc_to_string(node->loc);
    }
    // 関数ポインタの配列の場合 (変数宣言と同じく非対応)
    if (is_func_pointer_decl && this->token_kind_is(TK_LBRACKET)) {
        throw std::string("compiler error: array of function pointers is not supported at ")
              + loc_to_string(node->loc);
    }

    // 配列型のメンバ: 名前の後に [ 要素数 ] があれば固定長配列 (要素数省略・初期化子は非対応)
    if (this->token_kind_is(TK_LBRACKET)) {
        this->get_token(TK_LBRACKET);
        node->type.is_array = true;
        node->children.push_back(this->parse_expr());   // 配列サイズ (定数式，意味解析で畳み込む)
        this->get_token(TK_RBRACKET);
    }

    this->get_token(TK_SEMICOLON);
    return node;
}

// 構造体定義を解析する
// 構文: struct [構造体名] { メンバ宣言... } [変数名] ;
// 構造体名を省略した場合(無名構造体)は，その場での変数宣言を必須とする(再宣言する手段がないため)
// 戻り値: [0]=構造体定義(ND_STRUCT_DECL)．変数も宣言する場合は[1]に変数宣言(ND_VAR_DECL)を追加する
std::vector<node_t *> Parser::parse_struct_decl() {
    node_t *decl = this->new_node(ND_STRUCT_DECL);
    this->get_token(TK_STRUCT);

    // 構造体名 (省略可)
    const bool has_tag = this->token_kind_is(TK_IDENT);
    if (has_tag) {
        decl->sval = this->get_token().value;
    } else {
        // 無名構造体には，Pynesisソース中に識別子として書けない専用の名前を割り当てる
        // (識別子は英字/_で始まるトークンのみのため，$で始まる名前はソース上の構造体名と衝突しない)
        decl->sval = "$anon" + std::to_string(this->anon_struct_count_++);
    }

    this->get_token(TK_LBRACE);   // 開き波括弧
    while (!this->token_kind_is(TK_RBRACE)) {   // } が来るまでメンバ宣言を繰り返し読む
        decl->children.push_back(this->parse_struct_member());
    }
    this->get_token(TK_RBRACE);   // 閉じ波括弧

    std::vector<node_t *> result = {decl};

    // 構造体定義に続けて変数名があれば，その場で変数宣言も生成する (配列宣言も可)
    if (this->token_kind_is(TK_IDENT)) {
        node_t *var = this->new_node(ND_VAR_DECL);   // 即時宣言する変数
        var->type = {BASE_STRUCT, true};             // 型は今定義した構造体
        var->type.struct_name = decl->sval;          // 構造体名を結びつける
        var->sval = this->get_token().value;         // 変数名を読む
        // 変数名の後に[があれば配列宣言 (無ければ単一変数)
        if (this->token_kind_is(TK_LBRACKET)) {
            this->get_token(TK_LBRACKET);
            var->type.is_array = true;
            var->children.push_back(this->parse_expr());   // 配列サイズ (定数式，意味解析で畳み込む)
            this->get_token(TK_RBRACKET);
        }
        result.push_back(var);
    } else if (!has_tag) {
        throw std::string("compiler error: anonymous struct must declare a variable at ")
              + loc_to_string(this->peek_token().loc);
    }

    this->get_token(TK_SEMICOLON);
    return result;
}

// 式を解析してASTノードを返す
node_t *Parser::parse_expr() {
    // 最も優先順位の低い代入式から解析を開始する
    return this->parse_assign();
}

// 代入式 x = 式 (複合代入含む) を解析してASTノードを返す
// まず三項演算子以下の式として解析し，後ろに代入演算子が来ていたら代入式と判断する
node_t *Parser::parse_assign() {
    node_t *left = this->parse_ternary();

    // 代入演算子が来なければ代入式ではないのでそのまま返す(パススルー)
    if (!Parser::is_assign_op(this->peek_token().kind)) {
        return left;
    }

    // 代入式として処理する (右辺は再帰解析により右結合になる)
    // 左辺が代入できない式((a?b:c)=1 や (1+2)=x 等)でも，ここでは構文として通す．
    // 左辺が代入可能か(変数か)の検査は意味解析(analyze_exprのND_ASSIGN)に委ねる
    const token_t op = this->get_token();
    node_t *node = this->new_node(ND_ASSIGN);
    node->loc = op.loc;         // 演算子の位置を使う
    node->sval = op.value;      // 演算子の文字列 ("=", "+=" 等)
    node->children = {left, this->parse_assign()};
    return node;
}

// 三項演算子 a ? b : c を解析してASTノードを返す
// まず二項演算子の式として解析し，後ろに?が来ていたら三項演算子と判断する
node_t *Parser::parse_ternary() {
    node_t *cond = this->parse_binary(0);

    // ?が来なければ三項演算子ではないのでそのまま返す(パススルー)
    if (!this->token_kind_is(TK_QUESTION)) {
        return cond;
    }

    // 三項演算子として処理する (then/elseは再帰解析により右結合になる)
    node_t *node = this->new_node(ND_TERNARY);
    this->get_token();                              // ? を消費
    node_t *then_expr = this->parse_ternary();      // then節
    this->get_token(TK_COLON);                      // : を消費
    node_t *else_expr = this->parse_ternary();      // else節
    node->children = {cond, then_expr, else_expr};
    return node;
}

// 二項演算子を含む式を解析してASTノードを返す (優先順位min_prec以上の演算子を処理)
// 優先順位climbing法: 左辺を解析した後，min_prec以上の優先順位を持つ演算子が
// 続く限り読み進め，右辺は「演算子の優先順位+1」で再帰させることで左結合にする
node_t *Parser::parse_binary(int min_prec) {
    node_t *left = this->parse_unary();

    while (true) {
        // 現在のトークンが二項演算子か，優先順位表で調べる
        const auto it = g_binop_prec.find(this->peek_token().kind);
        // 優先順位表にないか，優先順位が探索対象の最低ラインよりも低いならスルー(ネストしない)
        if (it == g_binop_prec.end() || it->second < min_prec) break;

        // 演算子トークンを取得する
        const token_t op = this->get_token();

        // 右辺を「この演算子の優先順位+1」で解析する (左結合)
        node_t *right = this->parse_binary(it->second + 1);

        // 二項演算ノードにまとめ，新しい左辺とする
        node_t *node = this->new_node(ND_BINOP);
        node->loc = op.loc;         // 演算子の位置を使う
        node->sval = op.value;      // 演算子の文字列
        node->children = { left, right };
        left = node;
    }

    return left;
}

// 前置単項演算子を含む式を解析してASTノードを返す
// 演算子があれば消費してオペランドを再帰的に解析する
// (!!x や +-x のような連続にも対応するため．また，前置演算子と後置演算子が両方ついていた場合に対応するため)
node_t *Parser::parse_unary() {
    const token_kind_t kind = this->peek_token().kind;
    // 間接参照*・番地の取得&は，値ではなく番地を扱う専用のノードにする
    // (二項演算の*・&と同じトークンだが，式の先頭に現れるものは単項演算子として読む)
    if (kind == TK_STAR || kind == TK_AMP) {
        const token_t op = this->get_token();
        node_t *node = this->new_node(kind == TK_STAR ? ND_DEREF : ND_ADDR);
        node->loc = op.loc;
        node->children = { this->parse_unary() };   // オペランドを再帰解析
        return node;
    }
    // 前置単項演算子かどうか調べる
    if (kind == TK_MINUS || kind == TK_PLUS
     || kind == TK_BANG  || kind == TK_TILDE
     || kind == TK_PLUSPLUS || kind == TK_MINUSMINUS) {
        const token_t op = this->get_token();
        node_t *node = this->new_node(ND_UNOP);
        node->loc = op.loc;
        node->sval = op.value;
        node->children = { this->parse_unary() };   // オペランドを再帰解析
        return node;
    }

    // 前置演算子でなければ後置演算子・関数呼び出しの解析に委譲する
    return this->parse_postfix();
}

// 後置演算子・構造体メンバアクセス・配列添字・関数呼び出しを解析してASTノードを返す
// メンバアクセス(.member・->member)・配列添字([i])・関数呼び出し((...))は，arr[i].fp(x)や
// p->next->vのように連鎖しうるため，どれも現れなくなるまでループで読み進める
node_t *Parser::parse_postfix() {
    node_t *node = this->parse_primary();

    while (true) {
        // 構造体メンバアクセス: 変数名.メンバ名，構造体配列要素.メンバ名，または(*p).メンバ名 (ネスト構造体は非対応)
        if (this->token_kind_is(TK_DOT)) {
            // .の前は構造体変数の名前，配列要素(arr[i]・p[i]・s.items[i]等)，構造体ポインタの間接参照(*p)の
            // いずれかでなければならない．要素が構造体であるかは意味解析で確かめる
            // (例: (a+b).xは非対応．ネスト構造体が非対応のため，entry.sub.xのような多段の連鎖も現れない)
            if (node->kind != ND_VAR && node->kind != ND_DEREF && node->kind != ND_ARRAY_ACCESS) {
                throw std::string("compiler error: expected a struct variable before '.' at ")
                      + loc_to_string(this->peek_token().loc);
            }
            this->get_token(TK_DOT);                     // . を消費
            node = this->parse_member_name(node);        // 以降の連鎖判定の対象をこのメンバアクセス自身に置き換える
            continue;
        }

        // 構造体ポインタを通したメンバアクセス: p->メンバ名 は (*p).メンバ名 と同じ意味のため，間接参照を基底とする形で表す
        // (->の前に書ける式を構文では限らず，構造体へのポインタであるかは意味解析で確かめる)
        if (this->token_kind_is(TK_ARROW)) {
            const token_t arrow = this->get_token(TK_ARROW);   // -> を消費
            node_t *deref = this->new_node(ND_DEREF);          // 構造体ポインタの間接参照
            deref->loc = arrow.loc;
            deref->children.push_back(node);
            node = this->parse_member_name(deref);
            continue;
        }

        // 配列要素アクセス: 配列変数・ポインタ変数の名前[インデックス式]，またはメンバ・間接参照・配列要素[インデックス式]
        if (this->token_kind_is(TK_LBRACKET)) {
            // []の前は変数名・メンバアクセス・間接参照・配列要素のいずれかでなければならない (例: (a+b)[0]は非対応)
            if (node->kind != ND_VAR && node->kind != ND_MEMBER_ACCESS
                && node->kind != ND_DEREF && node->kind != ND_ARRAY_ACCESS) {
                throw std::string("compiler error: expected a variable name before '[' at ")
                      + loc_to_string(this->peek_token().loc);
            }
            this->get_token(TK_LBRACKET);           // [
            node_t *access = this->new_node(ND_ARRAY_ACCESS);  // 配列アクセスノード
            access->loc = node->loc;                           // 位置は基底の位置を引き継ぐ
            if (node->kind == ND_VAR) {
                // 配列変数・ポインタ変数: 名前で解決するのでインデックス式のみをchildren[0]に持つ
                access->sval = node->sval;
                access->children.push_back(this->parse_expr());
            } else {
                // 配列型・ポインタ型のメンバ(entry.name[i]等)・間接参照((*pp)[i])・ポインタの配列の要素(names[i][j]):
                // インデックス式(children[0])に加え，添字を付ける基底の式(children[1])を持たせて意味解析に解決させる
                access->children.push_back(this->parse_expr());
                access->children.push_back(node);
            }
            this->get_token(TK_RBRACKET);           // ]
            node = access;   // 以降の連鎖判定の対象をこの配列アクセス自身に置き換える
            continue;
        }

        // 関数呼び出し: 呼び出し先の式をchildren[0]に，引数の式をカンマ区切りでchildren[1]以降に格納する
        // 呼び出し先は，関数名(直接呼び出し)と関数ポインタ(変数・メンバ・間接参照・配列要素)のどちらにもなる．
        // どちらであるかは，名前を関数と変数のどちらに解決するかで決まるため意味解析で判定する
        if (this->token_kind_is(TK_LPAREN)) {
            // (の前は関数名・関数ポインタになりうる式でなければならない (例: (a+b)(1)は非対応)
            if (node->kind != ND_VAR && node->kind != ND_MEMBER_ACCESS
                && node->kind != ND_DEREF && node->kind != ND_ARRAY_ACCESS) {
                throw std::string("compiler error: expected function name before '(' at ")
                      + loc_to_string(this->peek_token().loc);
            }
            this->get_token();                  // (
            node_t *call = this->new_node(ND_CALL);
            call->loc = node->loc;
            call->children.push_back(node);     // 呼び出し先
            // 引数がある場合はカンマ区切りでパースする
            if (!this->token_kind_is(TK_RPAREN)) {
                call->children.push_back(this->parse_expr());
                while (!this->token_kind_is(TK_RPAREN)) {
                    this->get_token(TK_COMMA);    // , を消費
                    call->children.push_back(this->parse_expr());
                }
            }
            this->get_token(TK_RPAREN);         // )
            node = call;
            continue;
        }

        break;
    }

    // 後置インクリメント・デクリメント (値を返すだけの式に続けて連鎖させる意味がないため，ここで式を閉じる．
    // 対象が番地を持つ左辺値であるかは意味解析で確かめる)
    if (this->token_kind_is(TK_PLUSPLUS) || this->token_kind_is(TK_MINUSMINUS)) {
        const token_t op = this->get_token();
        node_t *post = this->new_node(ND_POST_UNOP);
        post->loc = op.loc;
        post->sval = op.value;
        post->children = { node };
        return post;
    }

    return node;
}

// .・->の直後のメンバ名を読み，baseを基底とするメンバアクセスのノードを返す
node_t *Parser::parse_member_name(node_t *base) {
    node_t *member = this->new_node(ND_MEMBER_ACCESS);  // メンバアクセスノード
    member->loc = base->loc;                            // 位置は基底の位置を引き継ぐ
    member->children.push_back(base);                   // このメンバが属する構造体を表す式を子に持つ
    member->sval = this->get_token(TK_IDENT).value;     // メンバ名
    return member;
}

// 基本式を解析してASTノードを返す
// 対応するもの: 整数リテラル・文字リテラル・文字列リテラル・nullptr・sizeof・変数参照・括弧式
node_t *Parser::parse_primary() {
    // nullptr (どのポインタも指していないことを表す定数)
    if (this->token_kind_is(TK_NULLPTR)) {
        node_t *node = this->new_node(ND_NULLPTR);
        this->get_token();
        return node;
    }

    // sizeof(型名 または 変数名)
    if (this->token_kind_is(TK_SIZEOF)) {
        return this->parse_sizeof();
    }

    // 組み込み関数print/scan/streq/strcopy (予約語のため専用トークンで判定する)
    if (this->token_kind_is(TK_PRINT))   return this->parse_print();
    if (this->token_kind_is(TK_SCAN))    return this->parse_scan();
    if (this->token_kind_is(TK_STREQ))   return this->parse_streq();
    if (this->token_kind_is(TK_STRCOPY)) return this->parse_strcopy();

    // 整数リテラル
    if (this->token_kind_is(TK_INT_LIT)) {
        node_t *node = this->new_node(ND_INT_LIT);
        node->ival = Parser::parse_int_literal(this->get_token());
        return node;
    }

    // 文字リテラル
    if (this->token_kind_is(TK_CHAR_LIT)) {
        node_t *node = this->new_node(ND_CHAR_LIT);
        node->ival = Parser::parse_char_literal(this->get_token().value);
        return node;
    }

    // 文字列リテラル (式中で使用: 匿名グローバル配列として扱われる)
    if (this->token_kind_is(TK_STRING_LIT)) {
        node_t *node = this->new_node(ND_STRING_LIT);
        node->sval = Parser::parse_string_literal(this->get_token().value);
        return node;
    }

    // 変数参照・関数呼び出し (print/scanは予約語のため専用トークンで上で処理済み)
    if (this->token_kind_is(TK_IDENT)) {
        node_t *node = this->new_node(ND_VAR);
        node->sval = this->get_token().value;
        return node;
    }

    // 括弧式: ( 式 )
    if (this->token_kind_is(TK_LPAREN)) {
        this->get_token();
        node_t *node = this->parse_expr();
        this->get_token(TK_RPAREN);
        return node;
    }

    throw std::string("compiler error: expected expression but got '")
          + this->peek_token().value + "' at " + loc_to_string(this->peek_token().loc);
}

// 整数リテラルのトークンを数値に変換する (0x/0X接頭辞があれば16進数，無ければ10進数)
// 16進は最上位ビットが立つ値(0x80000000以上)も書け，その型は意味解析でunsigned intになる
// long long(64bit)の範囲を超えるリテラルはstd::stollがstd::out_of_rangeを投げるため，ここで捕捉してコンパイルエラーに変換する
long long Parser::parse_int_literal(const token_t &token) {
    const std::string &text = token.value;   // リテラルの文字列
    // リテラルは32ビットに収まる値に限る．10進はint型の範囲(0〜2147483647)，16進は0xFFFFFFFFまで
    // (単項マイナスは別トークンとして扱われここでは付与されていないため，C言語同様リテラル自体の絶対値だけで判定する．
    //  そのため-2147483648(intの最小値)は10進では表現できない)
    const bool is_hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');   // 16進表記か
    const long long max_value = is_hex ? 0xFFFFFFFFLL : 2147483647LL;                                 // 書ける最大値
    long long value;
    try {
        value = std::stoll(is_hex ? text.substr(2) : text, nullptr, is_hex ? 16 : 10);
    } catch (const std::out_of_range &) {
        throw std::string("compiler error: integer literal out of range: ") + text
              + " at " + loc_to_string(token.loc);
    } catch (const std::invalid_argument &) {
        throw std::string("compiler error: invalid integer literal: ") + text
              + " at " + loc_to_string(token.loc);
    }
    if (value < 0 || value > max_value) {
        throw std::string("compiler error: integer literal out of range: ") + text
              + " at " + loc_to_string(token.loc);
    }
    return value;
}

// 文字リテラル文字列 ('a' や '\n' 等) を文字コードに変換する
long long Parser::parse_char_literal(const std::string &text) {
    // 引用符を取り除いた中身を取得する
    const std::string content = text.substr(1, text.size() - 2);

    // エスケープシーケンスでなければそのままの文字コードを返す
    if (content.size() == 1) {
        return static_cast<unsigned char>(content[0]);
    }

    // エスケープシーケンスを変換する
    switch (content[1]) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '0':  return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        default:   return static_cast<unsigned char>(content[1]);
    }
}

// 文字列リテラル(引用符除去済み)のエスケープシーケンスを解釈する
std::string Parser::parse_string_literal(const std::string &text) {
    std::string result;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            // エスケープシーケンスを変換する
            i++;
            switch (text[i]) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case '0':  result += '\0'; break;
                case '\\': result += '\\'; break;
                case '\'': result += '\''; break;
                case '"':  result += '"';  break;
                default:   result += text[i]; break;
            }
        } else {
            result += text[i];
        }
    }
    return result;
}
