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

// コンストラクタ: トークン列を受け取り，読み取り位置を初期化する
Parser::Parser(const std::vector<token_t> &tokens)
    : tokens_(tokens), pos_(0) {}

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
              + " at line " + std::to_string(actual.line);
    }
    return this->tokens_[this->pos_++];
}

// 現在のトークンの行番号でASTノードを生成する
node_t *Parser::new_node(node_kind_t kind) {
    node_t *node = new node_t;
    node->kind   = kind;
    node->line   = this->peek_token().line;
    node->ival   = 0;
    return node;
}

// 型の先頭になりうるトークン種別かどうか返す
bool Parser::is_type_start(token_kind_t kind) {
    return kind == TK_INT || kind == TK_CHAR || kind == TK_SHORT
        || kind == TK_SIGNED || kind == TK_UNSIGNED;
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
        case TK_IDENT:     return "identifier";
        case TK_EOF:       return "EOF";
        default:           return "token";
    }
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
        // void は変数型にならないため，必ず関数定義
        if (this->token_kind_is(TK_VOID)) {
            node->children.push_back(this->parse_func_def());
        }
        // 型キーワード(int/char/short等)で始まるなら関数定義またはグローバル変数宣言
        else if (Parser::is_type_start(this->peek_token().kind)) {
            // signed/unsignedがあれば，本体の型キーワードは1つ後ろにずれる
            const token_kind_t first_kind = this->peek_token().kind;
            const int offset = (first_kind == TK_SIGNED || first_kind == TK_UNSIGNED) ? 1 : 0;

            // 型の次が識別子でなければエラー (範囲外アクセスを避けてEOF扱いで判定する)
            if (this->peek_kind_ahead(offset + 1) != TK_IDENT) {
                throw std::string("compiler error: expected identifier after type at line ")
                      + std::to_string(this->peek_token().line);
            }
            // 識別子の次が '(' なら関数定義，それ以外は変数宣言
            if (this->peek_kind_ahead(offset + 2) == TK_LPAREN) {
                node->children.push_back(this->parse_func_def());
            } else {
                node->children.push_back(this->parse_var_decl());
            }
        }
        // それ以外がファイル直下にあるならエラー
        else {
            throw std::string("compiler error: expected function or variable declaration at line ")
                  + std::to_string(this->peek_token().line);
        }
    }

    return node;
}

// 関数定義を解析してND_FUNC_DEFを返す
// 構文: 戻り値型 関数名(void) ブロック
node_t *Parser::parse_func_def() {
    node_t *node = this->new_node(ND_FUNC_DEF);   // 関数ノード

    // signed/unsigned修飾子 (unsignedは未対応のためここで専用エラーにする)
    if (this->token_kind_is(TK_SIGNED)) {
        this->get_token();
    } else if (this->token_kind_is(TK_UNSIGNED)) {
        throw std::string("compiler error: 'unsigned' is not supported yet at line ")
              + std::to_string(this->peek_token().line);
    }

    // 戻り値型を読む
    base_type_t ret_base;
    const token_kind_t ret_kind = this->peek_token().kind;
    if      (ret_kind == TK_VOID)  { ret_base = BASE_VOID;  this->get_token(); }
    else if (ret_kind == TK_INT)   { ret_base = BASE_INT;   this->get_token(); }
    else if (ret_kind == TK_CHAR)  { ret_base = BASE_CHAR;  this->get_token(); }
    else if (ret_kind == TK_SHORT) { ret_base = BASE_SHORT; this->get_token(); }
    else {
        throw std::string("compiler error: expected return type at line ")
              + std::to_string(this->peek_token().line);
    }
    node->type = {ret_base, true};

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

// sizeof式を解析してND_SIZEOFを返す
// 構文: sizeof ( 型名 ) または sizeof ( 変数名 )  TODO: 任意の式には非対応
// 型名の場合はnode->typeに型を格納し(children空)，変数名の場合はchildren[0]にND_VARを格納する(意味解析で解決)
node_t *Parser::parse_sizeof() {
    node_t *node = this->new_node(ND_SIZEOF);
    this->get_token(TK_SIZEOF);                      // sizeof
    this->get_token(TK_LPAREN);                      // (

    const token_kind_t kind = this->peek_token().kind;
    if      (kind == TK_INT)   { node->type = {BASE_INT, true};   this->get_token(); }
    else if (kind == TK_CHAR)  { node->type = {BASE_CHAR, true};  this->get_token(); }
    else if (kind == TK_SHORT) { node->type = {BASE_SHORT, true}; this->get_token(); }
    else {
        // 型名でなければ変数名として解析する (意味解析で型を確定する)
        node_t *var = this->new_node(ND_VAR);
        var->sval = this->get_token(TK_IDENT).value;
        node->children.push_back(var);
    }

    this->get_token(TK_RPAREN);                      // )
    return node;
}

// 関数パラメータを解析してND_VAR_DECLを返す
// 構文: 型 変数名 (初期化子・セミコロンなし)
node_t *Parser::parse_param() {
    node_t *node = this->new_node(ND_VAR_DECL);

    // signed/unsigned修飾子 (unsignedは未対応のためここで専用エラーにする)
    if (this->token_kind_is(TK_SIGNED)) {
        this->get_token();
    } else if (this->token_kind_is(TK_UNSIGNED)) {
        throw std::string("compiler error: 'unsigned' is not supported yet at line ")
              + std::to_string(this->peek_token().line);
    }

    // パラメータの型
    base_type_t base;
    const token_kind_t kind = this->peek_token().kind;
    if      (kind == TK_INT)   { base = BASE_INT;   this->get_token(); }
    else if (kind == TK_CHAR)  { base = BASE_CHAR;  this->get_token(); }
    else if (kind == TK_SHORT) { base = BASE_SHORT; this->get_token(); }
    else {
        throw std::string("compiler error: expected parameter type at line ")
              + std::to_string(this->peek_token().line);
    }
    node->type = {base, true};

    // パラメータ名
    node->sval = this->get_token(TK_IDENT).value;

    // 配列パラメータ: 名前の後に [] があれば配列引数 (サイズ指定なし)
    if (this->token_kind_is(TK_LBRACKET)) {
        this->get_token(TK_LBRACKET);
        this->get_token(TK_RBRACKET);
        node->type.is_array = true;
    }

    return node;
}

// 変数宣言を解析してND_VAR_DECLを返す
// 構文: [signed|unsigned] 型 変数名 [= 式] ;
node_t *Parser::parse_var_decl() {
    node_t *node = this->new_node(ND_VAR_DECL);    // 変数宣言部

    // signed/unsigned 修飾子を読む (デフォルトは signed)
    // unsignedは予約語として受理するが当面未対応 (将来対応予定．is_signed等の符号情報の機構は残してある)
    bool is_signed = true;
    if (this->token_kind_is(TK_SIGNED)) {
        this->get_token();
    } else if (this->token_kind_is(TK_UNSIGNED)) {
        // this->get_token();
        // is_signed = false;
        throw std::string("compiler error: 'unsigned' is not supported yet at line ")
              + std::to_string(this->peek_token().line);
    }

    // 型キーワードを読む
    base_type_t base;
    const token_kind_t base_kind = this->peek_token().kind;
    if      (base_kind == TK_INT)   { base = BASE_INT;   this->get_token(); }
    else if (base_kind == TK_CHAR)  { base = BASE_CHAR;  this->get_token(); }
    else if (base_kind == TK_SHORT) { base = BASE_SHORT; this->get_token(); }
    else {
        throw std::string("compiler error: expected type at line ")
              + std::to_string(this->peek_token().line);
    }

    // 型情報と符号情報を保存
    node->type = {base, is_signed};

    // 変数名を読む
    node->sval = this->get_token(TK_IDENT).value;

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
    node->line = op.line;       // 演算子の行番号を使う
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
        node->line = op.line;       // 演算子の行番号を使う
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
    // 前置単項演算子かどうか調べる
    const token_kind_t kind = this->peek_token().kind;
    if (kind == TK_MINUS || kind == TK_PLUS
     || kind == TK_BANG  || kind == TK_TILDE
     || kind == TK_PLUSPLUS || kind == TK_MINUSMINUS) {
        const token_t op = this->get_token();
        node_t *node = this->new_node(ND_UNOP);
        node->line = op.line;
        node->sval = op.value;
        node->children = { this->parse_unary() };   // オペランドを再帰解析
        return node;
    }

    // 前置演算子でなければ後置演算子・関数呼び出しの解析に委譲する
    return this->parse_postfix();
}

// 後置演算子・関数呼び出しを解析してASTノードを返す
node_t *Parser::parse_postfix() {
    node_t *node = this->parse_primary();

    // 配列要素アクセス: 変数名[インデックス式]
    if (this->token_kind_is(TK_LBRACKET)) {
        // []の前は変数名でなければならない (例: (a+b)[0]は非対応)
        if (node->kind != ND_VAR) {
            throw std::string("compiler error: expected variable name before '[' at line ")
                  + std::to_string(this->peek_token().line);
        }
        this->get_token(TK_LBRACKET);           // [
        node_t *access = this->new_node(ND_ARRAY_ACCESS);
        access->line = node->line;
        access->sval = node->sval;              // 配列名
        access->children.push_back(this->parse_expr());   // インデックス式
        this->get_token(TK_RBRACKET);           // ]
        // 配列要素への後置++/--は非対応 (仕様上の制限)
        if (this->token_kind_is(TK_PLUSPLUS) || this->token_kind_is(TK_MINUSMINUS)) {
            throw std::string("compiler error: '++'/'--' on array element is not supported at line ")
                  + std::to_string(this->peek_token().line);
        }
        return access;
    }

    // 後置インクリメント・デクリメント
    if (this->token_kind_is(TK_PLUSPLUS) || this->token_kind_is(TK_MINUSMINUS)) {
        const token_t op = this->get_token();
        node_t *post = this->new_node(ND_POST_UNOP);
        post->line = op.line;
        post->sval = op.value;
        post->children = { node };
        return post;
    }

    // 関数呼び出し: 引数の式をカンマ区切りでchildrenに格納する
    if (this->token_kind_is(TK_LPAREN)) {
        // (の前は関数名でなければならない (例: (a+b)(1)は非対応)
        if (node->kind != ND_VAR) {
            throw std::string("compiler error: expected function name before '(' at line ")
                  + std::to_string(this->peek_token().line);
        }
        this->get_token();                  // (
        node_t *call = this->new_node(ND_CALL);
        call->line = node->line;
        call->sval = node->sval;            // 関数名
        // 引数がある場合はカンマ区切りでパースする
        if (!this->token_kind_is(TK_RPAREN)) {
            call->children.push_back(this->parse_expr());
            while (!this->token_kind_is(TK_RPAREN)) {
                this->get_token(TK_COMMA);    // , を消費
                call->children.push_back(this->parse_expr());
            }
        }
        this->get_token(TK_RPAREN);         // )
        return call;
    }

    return node;
}

// 基本式を解析してASTノードを返す
// 対応するもの: 整数リテラル・文字リテラル・文字列リテラル・sizeof・変数参照・括弧式
node_t *Parser::parse_primary() {
    // sizeof(型名 または 変数名)
    if (this->token_kind_is(TK_SIZEOF)) {
        return this->parse_sizeof();
    }

    // 組み込み関数print/scan (予約語のため専用トークンで判定する)
    if (this->token_kind_is(TK_PRINT)) return this->parse_print();
    if (this->token_kind_is(TK_SCAN))  return this->parse_scan();

    // 整数リテラル
    if (this->token_kind_is(TK_INT_LIT)) {
        node_t *node = this->new_node(ND_INT_LIT);
        node->ival = Parser::parse_int_literal(this->get_token().value);
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
          + this->peek_token().value + "' at line " + std::to_string(this->peek_token().line);
}

// 整数リテラル文字列を数値に変換する (0x/0X接頭辞があれば16進数，無ければ10進数)
// long long(64bit)の範囲を超えるリテラルはstd::stollがstd::out_of_rangeを投げるため，ここで捕捉してコンパイルエラーに変換する
long long Parser::parse_int_literal(const std::string &text) {
    long long value;
    try {
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            value = std::stoll(text.substr(2), nullptr, 16);
        } else {
            value = std::stoll(text, nullptr, 10);
        }
    } catch (const std::out_of_range &) {
        throw std::string("compiler error: integer literal out of range: ") + text;
    } catch (const std::invalid_argument &) {
        throw std::string("compiler error: invalid integer literal: ") + text;
    }
    // intは32ビットなので，リテラル自体はint型の範囲(0〜2147483647)に収まっているか検査する
    // (単項マイナスは別トークンとして扱われここでは付与されていないため，C言語同様リテラル自体の絶対値だけで判定する．
    //  そのため-2147483648(intの最小値)はこの言語では表現できない)
    if (value < 0 || value > 2147483647LL) {
        throw std::string("compiler error: integer literal out of range: ") + text;
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
