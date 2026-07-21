#include <algorithm>

#include "generator.hpp"

// 二項演算子の文字列を対応するアセンブリ命令に変換する
static std::string binop_mnemonic(const std::string &op) {
    if (op == "+") return "add";
    if (op == "-") return "sub";
    if (op == "*") return "mul";
    if (op == "&") return "and";
    if (op == "|") return "or";
    if (op == "^") return "xor";
    if (op == "/") return "div";    // 商 (符号付き除算．余りは捨てる)
    if (op == "<<") return "sll";   // 左シフト
    if (op == ">>") return "sra";   // 右シフト: unsigned非対応のため常に算術シフト(将来srlを符号で選択)
    // % は div の4引数形式で別途生成する．比較・論理演算子は分岐の段階で対応する
    throw std::string("compiler error: unsupported binary operator '") + op + "'";
}

// 比較演算子かどうかを返す
static bool is_comparison(const std::string &op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

// 比較演算子の「否定」に対応するF系命令を返す (偽のとき分岐させるのに使う)
static std::string negated_branch(const std::string &op) {
    if (op == "==") return "ne";    // ==の否定は!=
    if (op == "!=") return "eq";    // !=の否定は==
    if (op == "<")  return "egt";   // <の否定は>=
    if (op == ">")  return "elt";   // >の否定は<=
    if (op == "<=") return "gt";    // <=の否定は>
    if (op == ">=") return "lt";    // >=の否定は<
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 比較演算子に「そのまま」対応するF系命令を返す (真のとき分岐させるのに使う)
static std::string comparison_branch(const std::string &op) {
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<")  return "lt";
    if (op == ">")  return "gt";
    if (op == "<=") return "elt";
    if (op == ">=") return "egt";
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 命令出力の慣例:
// mov/rm/wm の即値モードではrs1(第1レジスタ)が無視される．
// 以降のコードでrs1の位置に書く r0 は値を持たないダミーであり，r0の値は読まれも書かれもしない．
// (例外: switchのディスパッチでは r0 に条件値を入れて実際に使う)

// 式の中に関数呼び出しが含まれるかどうかを再帰的に調べる
static bool contains_call(const node_t *expr) {
    if (expr->kind == ND_CALL) return true;
    for (const node_t *child : expr->children) {
        if (contains_call(child)) return true;
    }
    return false;
}

// コンストラクタ: AST・シンボルテーブル・パラメータシンボル表・構造体定義表・
// レジスタ退避領域の先頭番地・出力先を受け取る
Generator::Generator(node_t *root, const std::map<std::string, const symbol_t *> &symbols,
                     const std::map<std::string, std::vector<const symbol_t *>> &func_params,
                     const std::map<std::string, struct_def_t> &struct_defs,
                     int scratch_base, std::ofstream &asm_file)
    : root_(root), symbols_(symbols), func_params_(func_params), struct_defs_(struct_defs),
      scratch_base_(scratch_base), asm_file_(asm_file) {}

// コード生成を実行して .asm に書き出す
void Generator::operator()() {
    this->gen_program();
}

// プログラム全体を生成する
// .global宣言で全関数名を列挙し，main関数を先頭に各関数を出力する
void Generator::gen_program() {
    // .global宣言: 子を走査して全関数名を集める (アセンブリは定義前に全関数の宣言が必要)
    bool first = true;
    this->asm_file_ << ".global ";
    for (node_t *child : this->root_->children) {
        if (child->kind != ND_FUNC_DEF) continue;   // 関数定義のみ対象 (グローバル変数は除く)
        if (!first) this->asm_file_ << ", ";
        this->asm_file_ << child->sval;
        first = false;
    }
    this->asm_file_ << "\n";

    // main関数を先頭に出力する (アセンブリはmainを一番最初に書く必要がある)
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval == "main") {
            this->asm_file_ << "\n";
            this->gen_func(child);
            break;
        }
    }

    // main以外の関数を出力する
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval != "main") {
            this->asm_file_ << "\n";
            this->gen_func(child);
        }
    }
}

// グローバル変数の初期化コードを生成する
// プログラム直下の変数宣言を走査し，初期化子があるものの初期化命令を出力する
// (mainが最初に実行されるため，mainの先頭で呼び出す)
void Generator::gen_global_inits() {
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_VAR_DECL) {
            this->gen_var_decl(child);
        }
    }

    // 式中に現れる文字列リテラル(匿名グローバル配列)も，ここで1回だけ初期化する
    // (呼び出し回数に関わらず値が変わらない定数データのため，通常のグローバル変数と同じ扱い)
    std::vector<node_t *> string_lits;
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF) {
            this->collect_string_literals(child, string_lits);
        }
    }
    for (node_t *lit : string_lits) {
        this->gen_string_init(lit->sym->address, lit->sval);
    }
}

// AST全体(全関数の本体)を再帰的に走査し，式中に現れる文字列リテラル(匿名グローバル配列)を集める
// 変数宣言の初期化子として使われた文字列リテラルはsymを持たないため対象外
void Generator::collect_string_literals(node_t *node, std::vector<node_t *> &out) {
    if (node == nullptr) return;
    if (node->kind == ND_STRING_LIT && node->sym != nullptr) {
        out.push_back(node);
    }
    for (node_t *child : node->children) {
        this->collect_string_literals(child, out);
    }
}

// 関数定義を生成する
// 関数ラベルを出力し，本体ブロックの文を生成して，末尾にretを置く
void Generator::gen_func(node_t *func) {
    this->asm_file_ << func->sval << ":\n";
    // mainの先頭でグローバル変数を初期化する (mainが最初に実行されるため)
    if (func->sval == "main") {
        this->gen_global_inits();
    }
    this->gen_block(func->children.back());   // 本体ブロック(最後の子)の文を生成する
    this->asm_file_ << "    ret\n";       // 関数末尾のret (全関数にretが1つ以上必要)
}

// ブロックを生成する
// 中の文を上から順に生成する
void Generator::gen_block(node_t *block) {
    for (node_t *stmt : block->children) {
        this->gen_stmt(stmt);
    }
}

// 文を生成する
// 文の種別ごとに対応する生成処理へ振り分ける
void Generator::gen_stmt(node_t *stmt) {
    switch (stmt->kind) {
        // 変数宣言
        case ND_VAR_DECL:
            this->gen_var_decl(stmt);
            break;
        // 入れ子のブロック
        case ND_BLOCK:
            this->gen_block(stmt);
            break;
        // 代入文・関数呼び出し文・入出力文・増減文 (式文): 副作用のため評価する．結果(r0)は捨てる
        case ND_ASSIGN:
        case ND_CALL:
        case ND_PRINT:
        case ND_SCAN:
        case ND_STREQ:
        case ND_STRCOPY:
        case ND_UNOP:
        case ND_POST_UNOP:
            this->gen_expr(stmt, 0);
            break;
        // if文
        case ND_IF:
            this->gen_if(stmt);
            break;
        // while文
        case ND_WHILE:
            this->gen_while(stmt);
            break;
        // for文
        case ND_FOR:
            this->gen_for(stmt);
            break;
        // do-while文
        case ND_DO_WHILE:
            this->gen_do_while(stmt);
            break;
        // switch文
        case ND_SWITCH:
            this->gen_switch(stmt);
            break;
        // break文: 最内のループ/switchの脱出先へ飛ぶ (アナライザが内側であることを保証済み)
        case ND_BREAK:
            this->asm_file_ << "    jmp " << this->break_labels_.back() << "\n";
            break;
        // continue文: 最内ループの継続先へ飛ぶ
        case ND_CONTINUE:
            this->asm_file_ << "    jmp " << this->continue_labels_.back() << "\n";
            break;
        // return文: 戻り値があればRAX(r30)に書き込んでから復帰する
        case ND_RETURN:
            if (!stmt->children.empty()) {
                this->gen_expr(stmt->children[0], 0);                  // 戻り値の式 → r0
                this->asm_file_ << "    mov fh r0 " << RAX_REGISTER << "\n";  // r0 → RAX
            }
            this->asm_file_ << "    ret\n";
            break;
        default:
            break;
    }
}

// 変数宣言を生成する
// 初期化子があれば，初期値を変数の番地へ書き込むコードを生成する
void Generator::gen_var_decl(node_t *decl) {
    // 配列宣言: 文字列リテラルによる初期化のみ対応 (サイズ指定のみの宣言はスキップ)
    if (decl->type.is_array) {
        if (!decl->children.empty() && decl->children[0]->kind == ND_STRING_LIT) {
            this->gen_string_init(decl->sym->address, decl->children[0]->sval);
        }
        return;
    }
    // 初期化子がなければ何も出力しない (番地は確保済み，未初期化ローカルは不定値)
    if (decl->children.empty()) return;

    // 初期化式をr0に評価し，変数へ書き込む
    this->gen_expr(decl->children[0], 0);        // r0 = 初期値
    this->gen_store(0, decl->sym);               // 変数 = r0
}

// 文字列をchar配列のメモリに書き込む初期化コードを生成する
// 4文字ずつ1ワードにパックしてwmで書き込む (末尾にヌル終端を含む)
void Generator::gen_string_init(int base_addr, const std::string &str) {
    // ヌル終端を含めた全バイト列を構築する
    std::string data = str;
    data += '\0';
    // 4バイトずつワードにパックして書き込む
    const int word_count = (static_cast<int>(data.size()) + 3) / 4;
    for (int w = 0; w < word_count; w++) {
        // 1ワード = 4バイトをリトルエンディアン的にパックする (byte0が最下位)
        unsigned int word = 0;
        for (int b = 0; b < 4; b++) {
            const int idx = w * 4 + b;
            if (idx < static_cast<int>(data.size())) {
                word |= (static_cast<unsigned char>(data[idx]) << (b * 8));
            }
        }
        // ワードをメモリに書き込む (wワード目は base_addr + w*4 番地から4バイト)
        this->asm_file_ << "    mov fh r0 r0 " << word << "\n";
        this->asm_file_ << "    wm fh r0 r0 " << (base_addr + w * 4) << "\n";
    }
}

// char配列をヌル終端(0)まで1文字ずつ標準出力へ出力するループを生成する
// ヌル終端が見つからない不正な配列でも，配列の宣言サイズで安全に打ち切る
// レジスタ使用: r{reg}=インデックス, r{reg+1}=ベースアドレス(不変), r{reg+2}=アドレス→文字値(作業用),
//              r{reg+3}=配列サイズ(打ち切り境界，不変), r{reg+4}=0(ヌル終端比較用，不変), r{reg+5}=1(インデックス加算用，不変)
void Generator::gen_print_string(const symbol_t *sym, int reg) {
    if (reg + 5 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(reg + 1, sym);                                       // r{reg+1} = ベースアドレス
    this->asm_file_ << "    mov fh r0 r" << reg << " 0\n";                        // r{reg}   = インデックス(0)
    this->asm_file_ << "    mov fh r0 r" << (reg + 3) << " " << sym->type.array_size << "\n";  // r{reg+3} = 配列サイズ(打ち切り境界)
    this->asm_file_ << "    mov fh r0 r" << (reg + 4) << " 0\n";                  // r{reg+4} = 0 (ヌル終端比較用)
    this->asm_file_ << "    mov fh r0 r" << (reg + 5) << " 1\n";                  // r{reg+5} = 1 (インデックス加算用)

    this->asm_file_ << loop << ":\n";
    // 配列サイズに達したら打ち切る (ヌル終端がなくても無限ループ・範囲外読み出しを防ぐ)
    this->asm_file_ << "    egt r" << reg << " r" << (reg + 3) << " " << end << "\n";
    // r{reg+2} = mem[base + index] (1バイト)
    this->asm_file_ << "    add r" << (reg + 1) << " r" << reg << " r" << (reg + 2) << "\n";
    this->asm_file_ << "    rm 1h r" << (reg + 2) << " r" << (reg + 2) << "\n";
    // ヌル終端なら終了
    this->asm_file_ << "    eq r" << (reg + 2) << " r" << (reg + 4) << " " << end << "\n";
    this->asm_file_ << "    print r" << (reg + 2) << "\n";
    this->asm_file_ << "    add r" << reg << " r" << (reg + 5) << " r" << reg << "\n";
    this->asm_file_ << "    jmp " << loop << "\n";
    this->asm_file_ << end << ":\n";
}

// 標準入力を改行(\n=10)まで読み込み，char配列へヌル終端付きで格納するループを生成する
// 先頭の改行はすべて読み飛ばす．配列サイズ-1文字を超えたら追加のscanを行わず打ち切る
// (超過分は次にscanを実行したときに読み込まれる．そのためのscanが1回多く消費されることはない)
// レジスタ使用: r{reg}=読み込んだ文字, r{reg+1}=インデックス, r{reg+2}=ベースアドレス(不変), r{reg+3}=アドレス(作業用),
//              r{reg+4}='\n'(不変), r{reg+5}=配列サイズ-1(格納できる最大文字数，不変), r{reg+6}=1(インデックス加算用，不変),
//              r{reg+7}=0(ヌル終端書き込み用，不変)
void Generator::gen_scan_line(const symbol_t *sym, int reg) {
    if (reg + 7 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string skip_loop = this->new_label();
    const std::string skip_end = this->new_label();
    const std::string read_loop = this->new_label();
    const std::string read_end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(reg + 2, sym);                                              // r{reg+2} = ベースアドレス
    this->asm_file_ << "    mov fh r0 r" << (reg + 4) << " 10\n";                       // r{reg+4} = '\n'
    this->asm_file_ << "    mov fh r0 r" << (reg + 5) << " " << (sym->type.array_size - 1) << "\n";  // r{reg+5} = 配列サイズ-1
    this->asm_file_ << "    mov fh r0 r" << (reg + 6) << " 1\n";                        // r{reg+6} = 1
    this->asm_file_ << "    mov fh r0 r" << (reg + 7) << " 0\n";                        // r{reg+7} = 0

    // 先頭の改行はすべて読み飛ばす
    this->asm_file_ << "    scan r" << reg << "\n";
    this->asm_file_ << skip_loop << ":\n";
    this->asm_file_ << "    ne r" << reg << " r" << (reg + 4) << " " << skip_end << "\n";  // 改行以外ならスキップ終了
    this->asm_file_ << "    scan r" << reg << "\n";
    this->asm_file_ << "    jmp " << skip_loop << "\n";
    this->asm_file_ << skip_end << ":\n";

    // 改行が来るまで1文字ずつ配列へ格納する
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";                        // r{reg+1} = インデックス(0)
    this->asm_file_ << read_loop << ":\n";
    this->asm_file_ << "    eq r" << reg << " r" << (reg + 4) << " " << read_end << "\n";  // 改行なら終了
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    this->asm_file_ << "    wm 1h r" << (reg + 3) << " r" << reg << "\n";                // buf[index] = 文字
    this->asm_file_ << "    add r" << (reg + 1) << " r" << (reg + 6) << " r" << (reg + 1) << "\n";  // index += 1
    // 配列サイズ上限に達したら，これ以上scanせずに打ち切る (残りは次回のscanで読む)
    this->asm_file_ << "    egt r" << (reg + 1) << " r" << (reg + 5) << " " << read_end << "\n";
    this->asm_file_ << "    scan r" << reg << "\n";
    this->asm_file_ << "    jmp " << read_loop << "\n";
    this->asm_file_ << read_end << ":\n";

    // ヌル終端を書き込む
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    this->asm_file_ << "    wm 1h r" << (reg + 3) << " r" << (reg + 7) << "\n";           // buf[index] = 0
}

// char配列2つの内容を先頭から1文字ずつ比較し，一致すれば1，不一致なら0をr{reg}へ格納する
// どちらかの宣言サイズに達してもヌル終端が見つからない場合は，安全のため不一致として打ち切る
// レジスタ使用: r{reg}=結果, r{reg+1}=インデックス, r{reg+2}=ベースアドレスA(不変), r{reg+3}=ベースアドレスB(不変),
//              r{reg+4}=打ち切り境界(不変)，r{reg+5}=アドレス(作業用，A/B共用)，r{reg+6}=文字A(作業用)，
//              r{reg+7}=文字B(作業用)，r{reg+8}=1(インデックス加算用，不変)，r{reg+9}=0(ヌル終端比較用，不変)
void Generator::gen_streq(const symbol_t *sym_a, const symbol_t *sym_b, int reg) {
    if (reg + 9 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string mismatch = this->new_label();
    const std::string match = this->new_label();
    const std::string end = this->new_label();
    // 打ち切り境界は両配列の宣言サイズの小さい方(コンパイル時に確定するため1本のレジスタで表せる)
    const int size_limit = std::min(sym_a->type.array_size, sym_b->type.array_size);

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(reg + 2, sym_a);                          // r{reg+2} = ベースアドレスA
    this->gen_array_base_addr(reg + 3, sym_b);                          // r{reg+3} = ベースアドレスB
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";        // r{reg+1} = インデックス(0)
    this->asm_file_ << "    mov fh r0 r" << (reg + 4) << " " << size_limit << "\n";  // r{reg+4} = 打ち切り境界
    this->asm_file_ << "    mov fh r0 r" << (reg + 8) << " 1\n";        // r{reg+8} = 1(インデックス加算用)
    this->asm_file_ << "    mov fh r0 r" << (reg + 9) << " 0\n";        // r{reg+9} = 0(ヌル終端比較用)

    this->asm_file_ << loop << ":\n";
    this->asm_file_ << "    egt r" << (reg + 1) << " r" << (reg + 4) << " " << mismatch << "\n";  // 打ち切り境界に達したら不一致で終了
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 5) << "\n";  // アドレス = baseA+index
    this->asm_file_ << "    rm 1h r" << (reg + 5) << " r" << (reg + 6) << "\n";  // r{reg+6} = memA[アドレス] (1バイト)
    this->asm_file_ << "    add r" << (reg + 3) << " r" << (reg + 1) << " r" << (reg + 5) << "\n";  // アドレス = baseB+index
    this->asm_file_ << "    rm 1h r" << (reg + 5) << " r" << (reg + 7) << "\n";  // r{reg+7} = memB[アドレス] (1バイト)
    this->asm_file_ << "    ne r" << (reg + 6) << " r" << (reg + 7) << " " << mismatch << "\n";  // 文字が不一致なら終了
    this->asm_file_ << "    eq r" << (reg + 6) << " r" << (reg + 9) << " " << match << "\n";     // 両方ヌル終端なら一致で終了
    this->asm_file_ << "    add r" << (reg + 1) << " r" << (reg + 8) << " r" << (reg + 1) << "\n";  // index += 1
    this->asm_file_ << "    jmp " << loop << "\n";
    this->asm_file_ << match << ":\n";
    this->asm_file_ << "    mov fh r0 r" << reg << " 1\n";              // 一致
    this->asm_file_ << "    jmp " << end << "\n";
    this->asm_file_ << mismatch << ":\n";
    this->asm_file_ << "    mov fh r0 r" << reg << " 0\n";              // 不一致
    this->asm_file_ << end << ":\n";
}

// char配列srcの先頭からヌル終端まで1文字ずつdstへコピーする
// dstの宣言サイズ-1文字を超える場合は，そこで打ち切ってヌル終端を書き込む(scanと同じ安全な打ち切り)
// レジスタ使用: r{reg}=コピー中の文字(作業用), r{reg+1}=インデックス, r{reg+2}=ベースアドレス(コピー先，不変),
//              r{reg+3}=ベースアドレス(コピー元，不変), r{reg+4}=アドレス(作業用), r{reg+5}=0(ヌル終端書き込み用，不変),
//              r{reg+6}=コピー先の配列サイズ-1(打ち切り境界，不変), r{reg+7}=1(インデックス加算用，不変)
void Generator::gen_strcopy(const symbol_t *dst, const symbol_t *src, int reg) {
    if (reg + 7 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string truncate = this->new_label();
    const std::string finish = this->new_label();
    const std::string end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(reg + 2, dst);                            // r{reg+2} = ベースアドレス(コピー先)
    this->gen_array_base_addr(reg + 3, src);                            // r{reg+3} = ベースアドレス(コピー元)
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";        // r{reg+1} = インデックス(0)
    this->asm_file_ << "    mov fh r0 r" << (reg + 5) << " 0\n";        // r{reg+5} = 0(ヌル終端書き込み用)
    this->asm_file_ << "    mov fh r0 r" << (reg + 6) << " " << (dst->type.array_size - 1) << "\n";  // r{reg+6} = コピー先の配列サイズ-1
    this->asm_file_ << "    mov fh r0 r" << (reg + 7) << " 1\n";        // r{reg+7} = 1(インデックス加算用)

    this->asm_file_ << loop << ":\n";
    this->asm_file_ << "    egt r" << (reg + 1) << " r" << (reg + 6) << " " << truncate << "\n";  // コピー先の宣言サイズ-1文字に達したら打ち切る
    this->asm_file_ << "    add r" << (reg + 3) << " r" << (reg + 1) << " r" << (reg + 4) << "\n";  // アドレス = baseSrc+index
    this->asm_file_ << "    rm 1h r" << (reg + 4) << " r" << reg << "\n";  // r{reg} = memSrc[アドレス] (1バイト)
    this->asm_file_ << "    eq r" << reg << " r" << (reg + 5) << " " << finish << "\n";  // コピー元がヌル終端ならdstへも書いて終了
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 4) << "\n";  // アドレス = baseDst+index
    this->asm_file_ << "    wm 1h r" << (reg + 4) << " r" << reg << "\n";  // dst[index] = memSrcから読んだ文字
    this->asm_file_ << "    add r" << (reg + 1) << " r" << (reg + 7) << " r" << (reg + 1) << "\n";  // index += 1
    this->asm_file_ << "    jmp " << loop << "\n";
    this->asm_file_ << truncate << ":\n";
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 6) << " r" << (reg + 4) << "\n";  // アドレス = baseDst+(配列サイズ-1)
    this->asm_file_ << "    wm 1h r" << (reg + 4) << " r" << (reg + 5) << "\n";  // dstの末尾にヌル終端を書く
    this->asm_file_ << "    jmp " << end << "\n";
    this->asm_file_ << finish << ":\n";
    this->asm_file_ << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 4) << "\n";  // アドレス = baseDst+index
    this->asm_file_ << "    wm 1h r" << (reg + 4) << " r" << (reg + 5) << "\n";  // dstの現在位置にヌル終端を書く
    this->asm_file_ << end << ":\n";
}

// 一意な局所ラベル (.L0, .L1, ...) を生成して返す
std::string Generator::new_label() {
    return ".L" + std::to_string(this->label_count_++);
}

// 条件式condが偽のとき，labelへ分岐する命令を出力する
// 評価にはr{reg}・r{reg+1}を使う (式の途中で呼ばれても下位レジスタを壊さないため)
void Generator::gen_branch_if_false(node_t *cond, const std::string &label, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(cond->line);
    }
    // 比較条件: 否定したF系で「偽のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, reg);   // 右 → r{reg+1}
        this->asm_file_ << "    " << negated_branch(cond->sval)
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，0(偽)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                      // cond → r{reg}
        this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";    // r{reg+1} = 0
        this->asm_file_ << "    eq r" << reg << " r" << (reg + 1) << " " << label << "\n";  // 0なら飛ぶ
    }
}

// 条件式condが真のとき，labelへ分岐する命令を出力する
// 評価にはr{reg}・r{reg+1}を使う
void Generator::gen_branch_if_true(node_t *cond, const std::string &label, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(cond->line);
    }
    // 比較条件: そのままのF系で「真のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, reg);   // 右 → r{reg+1}
        this->asm_file_ << "    " << comparison_branch(cond->sval)
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，0でない(真)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                      // cond → r{reg}
        this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";    // r{reg+1} = 0
        this->asm_file_ << "    ne r" << reg << " r" << (reg + 1) << " " << label << "\n";  // 0以外なら飛ぶ
    }
}

// 比較演算を0/1の値としてr{reg}に生成する
// 左をr{reg}・右をr{reg+1}に評価し，比較が真なら1・偽なら0をr{reg}に置く
void Generator::gen_compare(node_t *expr, int reg) {
    const std::string t = this->new_label();      // 真の場合の飛び先
    const std::string end = this->new_label();
    this->gen_expr(expr->children[0], reg);        // 左 → r{reg}
    this->gen_expr_protecting(expr->children[1], reg + 1, reg);    // 右 → r{reg+1}
    // 比較が真なら .Lt へ
    this->asm_file_ << "    " << comparison_branch(expr->sval)
                    << " r" << reg << " r" << (reg + 1) << " " << t << "\n";
    this->asm_file_ << "    mov fh r0 r" << reg << " 0\n";   // 偽: r{reg} = 0
    this->asm_file_ << "    jmp " << end << "\n";
    this->asm_file_ << t << ":\n";
    this->asm_file_ << "    mov fh r0 r" << reg << " 1\n";   // 真: r{reg} = 1
    this->asm_file_ << end << ":\n";
}

// 論理 && / || を短絡評価し，結果(0/1)をr{reg}に生成する
// && は0で短絡(両方非0で1)，|| は非0で短絡(両方0で0)
void Generator::gen_logical(node_t *expr, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(expr->line);
    }
    const bool is_and = (expr->sval == "&&");
    const std::string shortcut = this->new_label();   // 短絡時の飛び先
    const std::string end = this->new_label();
    // 短絡判定のF系: && は「0なら短絡(eq)」, || は「0以外なら短絡(ne)」
    const std::string br = is_and ? "eq" : "ne";

    // 左を評価．短絡条件を満たせば右を評価する命令を飛ばして結果へ行く
    this->gen_expr(expr->children[0], reg);
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";
    this->asm_file_ << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // 右を評価．こちらは飛ばす対象が無いので短絡ではなく，結果(0/1)を確定させるための判定
    this->gen_expr(expr->children[1], reg);
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";
    this->asm_file_ << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // どちらも短絡しなかった場合の結果 (&&なら1, ||なら0)
    this->asm_file_ << "    mov fh r0 r" << reg << " " << (is_and ? 1 : 0) << "\n";
    this->asm_file_ << "    jmp " << end << "\n";
    // 短絡した場合の結果 (&&なら0, ||なら1)
    this->asm_file_ << shortcut << ":\n";
    this->asm_file_ << "    mov fh r0 r" << reg << " " << (is_and ? 0 : 1) << "\n";
    this->asm_file_ << end << ":\n";
}

// 三項演算子 a ? b : c の結果をr{reg}に生成する
void Generator::gen_ternary(node_t *expr, int reg) {
    const std::string else_label = this->new_label();
    const std::string end = this->new_label();
    this->gen_branch_if_false(expr->children[0], else_label, reg);   // 条件が偽ならelse値へ
    this->gen_expr(expr->children[1], reg);                          // then値 → r{reg}
    this->asm_file_ << "    jmp " << end << "\n";
    this->asm_file_ << else_label << ":\n";
    this->gen_expr(expr->children[2], reg);                          // else値 → r{reg}
    this->asm_file_ << end << ":\n";
}

// インクリメント/デクリメント (++/--) を生成する
// 対象は変数 (アナライザが読み書き可能を保証)．前置は増減後の新値・後置は増減前の旧値を式の値とする
void Generator::gen_incdec(node_t *expr, int reg, bool is_prefix) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(expr->line);
    }
    node_t *var = expr->children[0];                          // 対象変数 (ND_VAR)
    const std::string op = (expr->sval == "++") ? "+" : "-";  // ++→加算, --→減算

    // 現在値を読み，1を載せる
    this->gen_load(reg, var->sym);                                        // r{reg} = x
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 1\n";         // r{reg+1} = 1

    if (is_prefix) {
        // 前置 ++x/--x : r{reg}を増減して書き戻す (新値がそのまま式の値として残る)
        this->gen_binop_instr(op, reg, reg, reg + 1);                       // r{reg} = x ± 1
        this->gen_store(reg, var->sym);                                     // x = r{reg}
    } else {
        // 後置 x++/x-- : 旧値をr{reg}に残したまま，新値をr{reg+1}で計算して書き戻す
        this->gen_binop_instr(op, reg + 1, reg, reg + 1);                   // r{reg+1} = x ± 1
        this->gen_store(reg + 1, var->sym);                                 // x = r{reg+1}
    }
}

// 単項演算 (-/+/~/!) を生成する
void Generator::gen_unary(node_t *expr, int reg) {
    const std::string &op = expr->sval;

    this->gen_expr(expr->children[0], reg);   // オペランド → r{reg}

    if (op == "+") {
        // 単項+ : 値はオペランドそのもの (何もしない)
        return;
    }
    else if (op == "-") {
        // 単項- : 0 - x で符号反転する (r{reg+1}を使うため上限(r15)を超えないことを確認する)
        if (reg + 1 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at line ")
                  + std::to_string(expr->line);
        }
        this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";                          // r{reg+1} = 0
        this->asm_file_ << "    sub r" << (reg + 1) << " r" << reg << " r" << reg << "\n";    // r{reg} = 0 - x
    } else if (op == "~") {
        // ビット反転 : NOT命令 (not rs1 rd)
        this->asm_file_ << "    not r" << reg << " r" << reg << "\n";                         // r{reg} = ~x
    } else if (op == "!") {
        // 論理否定 : x==0 なら1，それ以外は0 (比較と同じ0/1生成パターン，r{reg+1}を使うため上限(r15)を超えないことを確認する)
        if (reg + 1 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at line ")
                  + std::to_string(expr->line);
        }
        const std::string t = this->new_label();      // 真(x==0)の飛び先
        const std::string end = this->new_label();
        this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " 0\n";                          // r{reg+1} = 0
        this->asm_file_ << "    eq r" << reg << " r" << (reg + 1) << " " << t << "\n";        // x==0 なら .Lt へ
        this->asm_file_ << "    mov fh r0 r" << reg << " 0\n";   // x!=0: r{reg} = 0
        this->asm_file_ << "    jmp " << end << "\n";
        this->asm_file_ << t << ":\n";
        this->asm_file_ << "    mov fh r0 r" << reg << " 1\n";   // x==0: r{reg} = 1
        this->asm_file_ << end << ":\n";
    } else {
        throw std::string("compiler error: unsupported unary operator '") + op
              + "' at line " + std::to_string(expr->line);
    }
}

// if文を生成する
// children: [0]=条件, [1]=then節, [2]=else節(省略可)
void Generator::gen_if(node_t *stmt) {
    node_t *cond = stmt->children[0];
    const bool has_else = stmt->children.size() == 3;

    if (!has_else) {
        // if (cond) then : 偽なら本体を飛ばす
        const std::string end = this->new_label();
        this->gen_branch_if_false(cond, end);
        this->gen_stmt(stmt->children[1]);
        this->asm_file_ << end << ":\n";
    } else {
        // if (cond) then else else節 : 偽ならelseへ，thenの後はelseを飛ばす
        const std::string else_label = this->new_label();
        const std::string end = this->new_label();
        this->gen_branch_if_false(cond, else_label);
        this->gen_stmt(stmt->children[1]);
        this->asm_file_ << "    jmp " << end << "\n";
        this->asm_file_ << else_label << ":\n";
        this->gen_stmt(stmt->children[2]);
        this->asm_file_ << end << ":\n";
    }
}

// while文を生成する
// children: [0]=条件, [1]=本体
void Generator::gen_while(node_t *stmt) {
    const std::string top = this->new_label();   // 条件判定の先頭 (continueの飛び先)
    const std::string end = this->new_label();   // ループ脱出先 (breakの飛び先)
    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(top);

    this->asm_file_ << top << ":\n";
    this->gen_branch_if_false(stmt->children[0], end);   // 条件が偽なら脱出
    this->gen_stmt(stmt->children[1]);                   // 本体
    this->asm_file_ << "    jmp " << top << "\n";        // 先頭(条件)へ戻る
    this->asm_file_ << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// for文を生成する
// children: [0]=初期化, [1]=条件, [2]=更新, [3]=本体 (各部は省略時nullptr)
void Generator::gen_for(node_t *stmt) {
    node_t *init = stmt->children[0];
    node_t *cond = stmt->children[1];
    node_t *update = stmt->children[2];
    node_t *body = stmt->children[3];

    const std::string top = this->new_label();    // 条件判定の先頭
    const std::string cont = this->new_label();   // continueの飛び先 (更新部)
    const std::string end = this->new_label();    // ループ脱出先 (break)

    // 初期化 (省略時はnullptr)．ループ前に1回だけ実行する
    if (init != nullptr) this->gen_stmt(init);

    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(cont);

    this->asm_file_ << top << ":\n";
    // 条件 (省略時は判定なし＝常にループ)
    if (cond != nullptr) this->gen_branch_if_false(cond, end);
    this->gen_stmt(body);                            // 本体
    this->asm_file_ << cont << ":\n";                // continueはここ(更新部)へ来る
    if (update != nullptr) this->gen_stmt(update);   // 更新 (省略可)
    this->asm_file_ << "    jmp " << top << "\n";    // 条件へ戻る
    this->asm_file_ << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// do-while文を生成する
// children: [0]=本体, [1]=条件 (本体を実行してから末尾で条件判定する)
void Generator::gen_do_while(node_t *stmt) {
    const std::string top = this->new_label();    // ループ先頭 (本体)
    const std::string cont = this->new_label();   // continueの飛び先 (末尾の条件判定)
    const std::string end = this->new_label();    // 脱出先 (break)
    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(cont);

    this->asm_file_ << top << ":\n";
    this->gen_stmt(stmt->children[0]);             // 本体
    this->asm_file_ << cont << ":\n";              // continueはここ(条件判定)へ来る
    this->gen_branch_if_true(stmt->children[1], top);   // 条件が真なら先頭へ戻る
    this->asm_file_ << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// switch文を生成する
// children: [0]=条件式, [1..]=case/defaultラベルと文を平坦に並べたもの
// 意味解析がcase値をivalに畳み込み済み
void Generator::gen_switch(node_t *stmt) {
    // 各case/defaultにラベルを割り当てる
    std::map<node_t *, std::string> label_of;   // case/defaultノード → 飛び先ラベル
    std::string default_label;                  // default節のラベル (無ければ空)
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE) {
            label_of[c] = this->new_label();
        } else if (c->kind == ND_DEFAULT) {
            default_label = this->new_label();
            label_of[c] = default_label;
        }
    }
    const std::string end = this->new_label();   // switch脱出先 (breakの飛び先)
    this->break_labels_.push_back(end);

    // ディスパッチ: 条件を一度r0に評価し，各caseと比較して一致したらそのラベルへ飛ぶ
    this->gen_expr(stmt->children[0], 0);   // r0 = 条件値
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE) {
            this->asm_file_ << "    mov fh r0 r1 " << c->ival << "\n";    // r1 = case値
            this->asm_file_ << "    eq r0 r1 " << label_of[c] << "\n";    // 一致ならそのcaseへ
        }
    }
    // どのcaseにも一致しなければ default へ (無ければ end へ)
    this->asm_file_ << "    jmp " << (default_label.empty() ? end : default_label) << "\n";

    // 本体: case/defaultラベルを所定位置に置き，文を順に生成する (フォールスルーは自然に表現される)
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE || c->kind == ND_DEFAULT) {
            this->asm_file_ << label_of[c] << ":\n";
        } else {
            this->gen_stmt(c);
        }
    }
    this->asm_file_ << end << ":\n";

    this->break_labels_.pop_back();
}

// r{dst} = r{lhs} op r{rhs} となる演算命令を出力する
// 二項演算と複合代入で共用する (剰余だけはdivの4引数形式)
void Generator::gen_binop_instr(const std::string &op, int dst, int lhs, int rhs) {
    // 剰余: divは商をrdへ・余りをimmが指すレジスタ番地へ格納する
    // 商をr{rhs}に捨て，余りをr{dst}(番地dst)へ得る
    if (op == "%") {
        this->asm_file_ << "    div r" << lhs << " r" << rhs
                        << " r" << rhs << " " << dst << "\n";
    } else {
        // それ以外は単一命令
        const std::string mn = binop_mnemonic(op);   // 演算子→命令
        this->asm_file_ << "    " << mn
                        << " r" << lhs << " r" << rhs << " r" << dst << "\n";
    }
}

// 変数の値をr{reg}へ読み込む
// 置き場所がレジスタ直結(LED等のI/Oレジスタ)ならmovのレジスタ間コピー，メモリ変数ならrm
// メモリ変数はchar/shortの型幅でmaskし(他バイトのゴミを混入させない)，符号付きなので読み込み後に符号拡張する
// (レジスタ上の演算は型に関係なく常に32ビットで行うため，char/shortはintに昇格した状態で保持する)
void Generator::gen_load(int reg, const symbol_t *sym) {
    if (sym->location == LOC_REGISTER) {
        // mov rs1=番地, rd=r{reg} : r{reg} = register[番地] (即値を付けないとレジスタ間コピーになる)
        this->asm_file_ << "    mov fh r" << sym->address << " r" << reg << "\n";
        return;
    }
    // 配列(配列パラメータ含む)はここでは要素の値ではなく「先頭アドレス」を保持しているだけなので，
    // 要素型に関わらず常にfh(全32ビット)で読み込む (char配列のアドレスを1バイトに切り詰めてはいけない)
    if (sym->type.is_array) {
        this->asm_file_ << "    rm fh r0 r" << reg << " " << sym->address << "\n";
        return;
    }
    switch (sym->type.base) {
        case BASE_CHAR:
            this->asm_file_ << "    rm 1h r0 r" << reg << " " << sym->address << "\n";
            this->gen_sign_extend(reg, 8);
            break;
        case BASE_SHORT:
            this->asm_file_ << "    rm 3h r0 r" << reg << " " << sym->address << "\n";
            this->gen_sign_extend(reg, 16);
            break;
        case BASE_INT:
            // rm: メモリ絶対番地からr{reg}へ読み込む (即値アドレス指定のためrs1のr0は無視される)
            this->asm_file_ << "    rm fh r0 r" << reg << " " << sym->address << "\n";
            break;
        default:
            throw std::string("compiler error: unsupported scalar type in gen_load");
    }
}

// r{reg}の値を変数へ書き込む
// 置き場所がレジスタ直結(LED等のI/Oレジスタ)ならmovのレジスタ間コピー，メモリ変数ならwm
// メモリ変数はchar/shortの型幅でmaskし，該当バイトのみ書き込む(桁あふれした上位ビットを書き込まない)
void Generator::gen_store(int reg, const symbol_t *sym) {
    if (sym->location == LOC_REGISTER) {
        // mov rs1=r{reg}, rd=番地 : register[番地] = r{reg}
        this->asm_file_ << "    mov fh r" << reg << " r" << sym->address << "\n";
        return;
    }
    // 配列(配列パラメータ含む)はここでは要素の値ではなく「先頭アドレス」を保持しているだけなので，
    // 要素型に関わらず常にfh(全32ビット)で書き込む (char配列のアドレスを1バイトに切り詰めてはいけない)
    if (sym->type.is_array) {
        this->asm_file_ << "    wm fh r0 r" << reg << " " << sym->address << "\n";
        return;
    }
    const char *mask;
    switch (sym->type.base) {
        case BASE_CHAR:  mask = "1h"; break;
        case BASE_SHORT: mask = "3h"; break;
        case BASE_INT:   mask = "fh"; break;
        default:
            throw std::string("compiler error: unsupported scalar type in gen_store");
    }
    // wm: r{reg}をメモリ絶対番地へ書き込む (即値アドレス指定のためrs1のr0は無視される)
    this->asm_file_ << "    wm " << mask << " r0 r" << reg << " " << sym->address << "\n";
}

// r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング，結果は同じレジスタに上書き)
// gen_loadのメモリ変数分岐と同じマスク・符号拡張の手順を，即値アドレスではなくレジスタが持つ実行時アドレスに適用する
void Generator::gen_load_indirect(int reg, const type_t &type) {
    switch (type.base) {
        case BASE_CHAR:
            this->asm_file_ << "    rm 1h r" << reg << " r" << reg << "\n";
            this->gen_sign_extend(reg, 8);
            break;
        case BASE_SHORT:
            this->asm_file_ << "    rm 3h r" << reg << " r" << reg << "\n";
            this->gen_sign_extend(reg, 16);
            break;
        case BASE_INT:
            this->asm_file_ << "    rm fh r" << reg << " r" << reg << "\n";
            break;
        default:
            throw std::string("compiler error: unsupported scalar type in gen_load_indirect");
    }
}

// r{val_reg}の値を，r{addr_reg}が指すメモリ番地へ型に応じたマスクで書き込む(レジスタ間接アドレッシング)
void Generator::gen_store_indirect(int addr_reg, int val_reg, const type_t &type) {
    const char *mask;
    switch (type.base) {
        case BASE_CHAR:  mask = "1h"; break;
        case BASE_SHORT: mask = "3h"; break;
        case BASE_INT:   mask = "fh"; break;
        default:
            throw std::string("compiler error: unsupported scalar type in gen_store_indirect");
    }
    this->asm_file_ << "    wm " << mask << " r" << addr_reg << " r" << val_reg << "\n";
}

// r{reg}の下位bitsビットを符号として32ビットへ符号拡張する
void Generator::gen_sign_extend(int reg, int bits) {
    const int shift = 32 - bits;
    // シフト量を保存しておく
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " " << shift << "\n";
    // 最上位ビットをMSBにシフトする
    this->asm_file_ << "    sll r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // 算術シフトして，実際の値が入っているよりも上位のビットを符号ビットで埋める
    this->asm_file_ << "    sra r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// 式を評価し結果を指定レジスタに残す．評価対象の式が関数呼び出しを含む場合，
// 呼び出し先はr0から使い直すため，別に指定したレジスタの値を一時メモリへ退避してから評価し，評価後に復元する
void Generator::gen_expr_protecting(node_t *expr, int reg, int protect_reg) {
    if (!contains_call(expr)) {
        this->gen_expr(expr, reg);
        return;
    }
    const int addr = this->scratch_base_ + protect_reg * 4;
    this->asm_file_ << "    wm fh r0 r" << protect_reg << " " << addr << "\n";   // 退避
    this->gen_expr(expr, reg);
    this->asm_file_ << "    rm fh r0 r" << protect_reg << " " << addr << "\n";   // 復元
}

// 式を評価し，結果をr{reg}に残す
// reg以上のレジスタを作業用に使うレジスタスタック方式 (二項演算は左をr{reg}・右をr{reg+1}に評価して畳む)
void Generator::gen_expr(node_t *expr, int reg) {
    // レジスタは16本(r0〜r15)．深い式で枯渇したらエラーにする
    if (reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(expr->line);
    }

    switch (expr->kind) {
        // リテラル: 即値をr{reg}に載せる
        // sizeofは意味解析でコンパイル時に値(ival)が確定済みのため，リテラルと同じ即値ロードで済む
        case ND_INT_LIT:
        case ND_CHAR_LIT:
        case ND_SIZEOF:
            this->asm_file_ << "    mov fh r0 r" << reg << " " << expr->ival << "\n";
            break;

        // 文字列リテラル: 配列名と同様，先頭の番地(コンパイル時確定の即値)をr{reg}に載せる
        // データ自体はgen_global_initsで1回だけ書き込み済み
        // 現状は関数の引数としてしか使用されないので，先頭アドレスだけ保存すればいい
        case ND_STRING_LIT:
            this->asm_file_ << "    mov fh r0 r" << reg << " " << expr->sym->address << "\n";
            break;

        // 変数参照: 変数の値をr{reg}へ読み込む
        case ND_VAR:
            this->gen_load(reg, expr->sym);
            break;

        // 構造体メンバ参照:
        // 単一の構造体変数のメンバは，意味解析が「構造体変数の番地+メンバのオフセット」を
        // 1つのシンボルに合成済みのため，通常の変数参照と同じ経路で読み込める．
        // 構造体配列要素のメンバ(arr[i].member)は番地が実行時に決まるため，
        // アドレスを計算してからレジスタ間接で読み込む
        case ND_MEMBER_ACCESS:
            if (expr->children[0]->kind == ND_ARRAY_ACCESS) {
                this->gen_struct_array_member_addr(expr, reg);
                this->gen_load_indirect(reg, expr->type);
            } else {
                this->gen_load(reg, expr->sym);
            }
            break;

        // 二項演算
        case ND_BINOP:
            // 比較は0/1の値を生成，論理&&/||は短絡評価，それ以外は単一命令(+など)で畳む
            if (is_comparison(expr->sval)) {
                this->gen_compare(expr, reg);
            } else if (expr->sval == "&&" || expr->sval == "||") {
                this->gen_logical(expr, reg);
            } else {
                this->gen_expr(expr->children[0], reg);
                this->gen_expr_protecting(expr->children[1], reg + 1, reg);
                this->gen_binop_instr(expr->sval, reg, reg, reg + 1);   // r{reg} = r{reg} op r{reg+1}
            }
            break;

        // 三項演算子 a ? b : c
        case ND_TERNARY:
            this->gen_ternary(expr, reg);
            break;

        // 前置単項演算: ++/--は増減，それ以外(-/+/~/!)は単項演算
        case ND_UNOP:
            if (expr->sval == "++" || expr->sval == "--") {
                this->gen_incdec(expr, reg, true);    // 前置
            } else {
                this->gen_unary(expr, reg);
            }
            break;

        // 後置単項演算: ++/--のみ (parserがND_POST_UNOPを作るのはこの2つだけ)
        case ND_POST_UNOP:
            this->gen_incdec(expr, reg, false);       // 後置
            break;

        // 関数呼び出し: 各引数をr{reg}で評価しパラメータのアドレスへ書き込んでからCALLする
        // callでレジスタは揮発するが，呼び出し前後で生きた値はメモリにあるため問題ない
        // r{reg}を使うのは，呼び出し元がr{reg}未満のレジスタに置いている生存値(二項演算の左辺等)を
        // 破壊しないため．各引数はメモリへの書き込みが完了してから次の引数評価に移るので使い回して良い
        case ND_CALL: {
            // 各引数をr{reg}に評価し，対応するパラメータのメモリアドレスにWMで書き込む
            const auto &params = this->func_params_.at(expr->sval);
            for (size_t i = 0; i < expr->children.size(); i++) {
                node_t *arg = expr->children[i];
                if (params[i]->type.is_array) {
                    // 配列引数: ベースアドレスをr{reg}にロードする
                    this->gen_array_base_addr(reg, arg->sym);
                } else {
                    // スカラー引数: 式を評価する
                    this->gen_expr(arg, reg);
                }
                this->gen_store(reg, params[i]);
            }
            this->asm_file_ << "    call " << expr->sval << "\n";
            // 非void関数はRAX(r30)から戻り値を取り出す
            if (expr->type.base != BASE_VOID) {
                this->asm_file_ << "    mov fh " << RAX_REGISTER << " r" << reg << "\n";
            }
            break;
        }

        // 組み込み関数print: char配列をヌル終端まで1文字ずつ出力するループを生成する
        case ND_PRINT:
            this->gen_print_string(expr->children[0]->sym, reg);
            break;

        // 組み込み関数scan: 標準入力を改行まで読み込み，char配列へヌル終端付きで格納するループを生成する
        case ND_SCAN:
            this->gen_scan_line(expr->children[0]->sym, reg);
            break;

        // 組み込み関数streq: 2つのchar配列の内容を比較し，一致すれば1，不一致なら0をr{reg}に生成する
        case ND_STREQ:
            this->gen_streq(expr->children[0]->sym, expr->children[1]->sym, reg);
            break;

        // 組み込み関数strcopy: 第2引数(src)の内容を第1引数(dst)へヌル終端付きでコピーする
        case ND_STRCOPY:
            this->gen_strcopy(expr->children[0]->sym, expr->children[1]->sym, reg);
            break;

        // 代入: 右辺(複合代入は左辺の現在値と右辺の演算結果)をr{reg}に求め，変数へ書き込む
        case ND_ASSIGN: {
            node_t *lhs = expr->children[0];
            if (lhs->kind == ND_ARRAY_ACCESS) {
                // 配列要素への代入
                this->gen_expr(expr->children[1], reg);         // 右辺 → r{reg}
                this->gen_array_store(lhs, reg, reg + 1);       // 配列要素へ書き込む
            } else if (lhs->kind == ND_MEMBER_ACCESS && lhs->children[0]->kind == ND_ARRAY_ACCESS) {
                // 構造体配列要素のメンバへの代入: 番地が実行時計算のため，アドレスを求めてから読み書きする．
                // 式の評価はreg以上のレジスタしか使わない規約(レジスタスタック方式)のため，
                // アドレスは右辺(または現在値の読み出し)より後ろのレジスタ(r{reg+1})に置く
                // (先にr{reg+1}へアドレスを置いてしまうと，reg起点で評価する式が
                //  自身の作業用としてr{reg+1}を使い，アドレスを上書きしてしまう)．
                // レジスタ使用: r{reg}=右辺値/現在値，r{reg+1}=アドレス(作業用にr{reg+2}も使う)
                if (reg + 2 >= MAX_REG) {
                    throw std::string("compiler error: expression too complex (out of registers) at line ")
                          + std::to_string(expr->line);
                }
                if (expr->sval == "=") {
                    // 単純代入: 右辺を先にr{reg}へ評価してから，アドレスをr{reg+1}へ求める(r{reg}を保護)
                    this->gen_expr(expr->children[1], reg);
                    this->gen_struct_array_member_addr(lhs, reg + 1, reg);
                } else {
                    // 複合代入 x op= e : アドレスをr{reg+1}へ求め，現在値をr{reg}へ読む．
                    // 右辺の評価が関数呼び出しを含む場合，呼び出し先はr0から使い直すため
                    // r{reg}(現在値)・r{reg+1}(アドレス)の両方が破壊されうる．
                    // gen_expr_protectingは1本のレジスタしか保護できないため，
                    // 2本とも退避してから評価し，あとで復元する
                    this->gen_struct_array_member_addr(lhs, reg + 1);
                    this->asm_file_ << "    mov fh r" << (reg + 1) << " r" << reg << "\n";  // r{reg} = アドレス
                    this->gen_load_indirect(reg, lhs->type);                                // r{reg} = 現在値
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);    // "+=" → "+"
                    if (contains_call(expr->children[1])) {
                        const int addr0 = this->scratch_base_ + reg * 4;
                        const int addr1 = this->scratch_base_ + (reg + 1) * 4;
                        this->asm_file_ << "    wm fh r0 r" << reg << " " << addr0 << "\n";        // 退避
                        this->asm_file_ << "    wm fh r0 r" << (reg + 1) << " " << addr1 << "\n";  // 退避
                        this->gen_expr(expr->children[1], reg + 2);                                // 右辺 → r{reg+2}
                        this->asm_file_ << "    rm fh r0 r" << reg << " " << addr0 << "\n";        // 復元
                        this->asm_file_ << "    rm fh r0 r" << (reg + 1) << " " << addr1 << "\n";  // 復元
                    } else {
                        this->gen_expr(expr->children[1], reg + 2);                                // 右辺 → r{reg+2}
                    }
                    this->gen_binop_instr(op, reg, reg, reg + 2);
                }
                this->gen_store_indirect(reg + 1, reg, lhs->type);
            } else {
                // スカラー変数への代入
                if (expr->sval == "=") {
                    // 単純代入: 右辺をr{reg}に評価する
                    this->gen_expr(expr->children[1], reg);
                } else {
                    // 複合代入 x op= e : 左辺の現在値をr{reg}・右辺をr{reg+1}に評価し，opで畳む
                    this->gen_load(reg, lhs->sym);
                    this->gen_expr_protecting(expr->children[1], reg + 1, reg);
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);   // "+=" → "+"
                    this->gen_binop_instr(op, reg, reg, reg + 1);
                }
                // 変数へ書き込む (代入式の値もr{reg}に残る)
                this->gen_store(reg, lhs->sym);
            }
            break;
        }

        // 配列要素アクセス(読み出し): インデックスからメモリアドレスを計算して読み込む
        case ND_ARRAY_ACCESS:
            this->gen_array_load(expr, reg);
            break;

        default:
            throw std::string("compiler error: unsupported expression in code generation at line ")
                  + std::to_string(expr->line);
    }
}

// 配列の先頭アドレスをr{reg}に載せる
// 直接配列(array_size>0)はコンパイル時にアドレス確定済みなので即値ロード，
// 配列パラメータ(array_size==0)は呼び出し元が書き込んだ先頭アドレスをメモリから間接読み出しする
void Generator::gen_array_base_addr(int reg, const symbol_t *sym) {
    if (sym->type.array_size == 0) {
        this->asm_file_ << "    rm fh r0 r" << reg << " " << sym->address << "\n";
    } else {
        this->asm_file_ << "    mov fh r0 r" << reg << " " << sym->address << "\n";
    }
}

// 構造体配列要素のメンバ(arr[i].member)の実アドレスをr{reg}に計算する
// アドレス = 配列先頭番地 + メンバオフセット(コンパイル時定数，member_accessが合成時にexpr->ivalへ保存済み)
//          + インデックス(実行時，member_access->children[0]->children[0]) × 構造体1要素分のバイト数
// レジスタ使用: r{reg}=インデックス→アドレス, r{reg+1}=定数(要素間隔・ベース，作業用)
void Generator::gen_struct_array_member_addr(node_t *member_access, int reg, int protect_reg) {
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(member_access->line);
    }
    const symbol_t *arr_sym = member_access->sym;          // 配列全体のシンボル(先頭番地・構造体名)
    node_t *array_access = member_access->children[0];     // arr[i] (children[0]=インデックス式)
    const int stride_bytes = this->struct_defs_.at(arr_sym->type.struct_name).total_words * 4;
    const int base_const = arr_sym->address + static_cast<int>(member_access->ival) * 4;  // 配列先頭+メンバオフセット

    // r{reg} = インデックス式 (protect_regが指定されていれば，その値を評価中も保護する)
    if (protect_reg >= 0) {
        this->gen_expr_protecting(array_access->children[0], reg, protect_reg);
    } else {
        this->gen_expr(array_access->children[0], reg);
    }
    // r{reg+1} = 要素間隔(構造体1要素分のバイト数．2の冪とは限らないためmulで乗算する)
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " " << stride_bytes << "\n";
    this->asm_file_ << "    mul r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // r{reg+1} = 配列先頭+メンバオフセット(コンパイル時定数)
    this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " " << base_const << "\n";
    // r{reg} = 実アドレス
    this->asm_file_ << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// 構造体メンバ配列アクセス(children.size()==2のND_ARRAY_ACCESS)の配列先頭アドレスをr{addr_reg}に載せる
// 通常の単一構造体変数のメンバ配列(entry.name[i])はコンパイル時アドレス確定，
// 構造体配列要素のメンバ配列(arr[i].name[j])は実行時アドレス計算になるため分岐する
void Generator::gen_member_array_base(node_t *expr, int addr_reg, int protect_reg) {
    node_t *designator = expr->children[1];   // ND_MEMBER_ACCESS
    if (designator->children[0]->kind == ND_ARRAY_ACCESS) {
        this->gen_struct_array_member_addr(designator, addr_reg, protect_reg);
    } else {
        this->gen_array_base_addr(addr_reg, designator->sym);
    }
}

// 配列要素をr{reg}へ読み込む
// アドレス = base + index * サイズ(バイト単位)をr{reg}に作り，型に応じたmaskで1回のrmで読み込む
// (maskの最下位ビットに合わせて自動的にレジスタLSB側へゼロ拡張格納されるため，シフトは不要)
// レジスタ使用: r{reg}=index→アドレス→結果, r{reg+1}=定数・base (2本．構造体配列要素のメンバ配列の場合，
// ベースアドレス計算(gen_struct_array_member_addr)がさらにr{reg+2}を使うため3本必要，reg<=13)
void Generator::gen_array_load(node_t *expr, int reg) {
    if (reg + 2 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(expr->line);
    }

    // 要素型は，通常配列ならexpr->symの型，構造体メンバ配列ならメンバ自身の型(children[1]->type)
    const base_type_t elem_base =
        (expr->children.size() == 2) ? expr->children[1]->type.base : expr->sym->type.base;

    // r{reg} = index (r{reg+1}はまだ未使用)
    this->gen_expr(expr->children[0], reg);

    // 型ごとの要素サイズ(2^shift バイト)とmask(バイト位置)を決定する
    const char *mask;
    int shift;
    switch (elem_base) {
        case BASE_CHAR:  mask = "1h"; shift = 0; break;
        case BASE_SHORT: mask = "3h"; shift = 1; break;
        case BASE_INT:   mask = "fh"; shift = 2; break;
        default:
            throw std::string("compiler error: unsupported array element type at line ")
                  + std::to_string(expr->line);
    }

    // オフセット = index * サイズ (サイズ1のcharはシフト不要)
    // 実行後: r{reg} = index * サイズ(バイトオフセット), r{reg+1} = シフト量(破棄可)
    if (shift > 0) {
        this->asm_file_ << "    mov fh r0 r" << (reg + 1) << " " << shift << "\n";
        this->asm_file_ << "    sll r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    }
    // アドレス = base + オフセット
    // 実行後: r{reg+1} = base番地 (通常配列・単一構造体メンバ配列はコンパイル時定数，
    //         構造体配列要素のメンバ配列は実行時計算．r{reg}を保護しつつr{reg+2}を作業用に使う)
    if (expr->children.size() == 2) {
        this->gen_member_array_base(expr, reg + 1, reg);
    } else {
        this->gen_array_base_addr(reg + 1, expr->sym);
    }
    // 実行後: r{reg} = base + オフセット = 読み込み先の実アドレス
    this->asm_file_ << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // maskに従って読み込む (該当バイトのみ自動抽出・ゼロ拡張)
    // 実行後: r{reg} = 配列要素の値 (式全体の結果)
    this->asm_file_ << "    rm " << mask << " r" << reg << " r" << reg << "\n";

    // char/shortはゼロ拡張されたままなので，スカラー変数の読み込み(gen_load)と同様に符号拡張する
    if (elem_base == BASE_CHAR) {
        this->gen_sign_extend(reg, 8);
    } else if (elem_base == BASE_SHORT) {
        this->gen_sign_extend(reg, 16);
    }
}

// r{val_reg}の値を配列要素へ書き込む (work_reg以降を作業用に使う)
// アドレス = base + index * サイズ(バイト単位)をr{work_reg}に作り，型に応じたmaskで1回のwmで書き込む
// (該当バイト以外はハードウェアが元の値を保持するread-modify-writeを内部で行うため，ソフト側での読み出しは不要)
// レジスタ使用: r{val_reg}=値, r{work_reg}=index→アドレス, r{work_reg+1}=定数・base (2本．構造体配列要素の
// メンバ配列の場合，ベースアドレス計算がさらにr{work_reg+2}を使うため3本必要，work_reg+2<=15)
void Generator::gen_array_store(node_t *expr, int val_reg, int work_reg) {
    if (work_reg + 2 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at line ")
              + std::to_string(expr->line);
    }

    // 要素型は，通常配列ならexpr->symの型，構造体メンバ配列ならメンバ自身の型(children[1]->type)
    const base_type_t elem_base =
        (expr->children.size() == 2) ? expr->children[1]->type.base : expr->sym->type.base;

    // r{work_reg} = index (r{work_reg+1}はまだ未使用)
    this->gen_expr_protecting(expr->children[0], work_reg, val_reg);

    // 型ごとの要素サイズ(2^shift バイト)とmask(バイト位置)を決定する
    const char *mask;
    int shift;
    switch (elem_base) {
        case BASE_CHAR:  mask = "1h"; shift = 0; break;
        case BASE_SHORT: mask = "3h"; shift = 1; break;
        case BASE_INT:   mask = "fh"; shift = 2; break;
        default:
            throw std::string("compiler error: unsupported array element type at line ")
                  + std::to_string(expr->line);
    }

    // オフセット = index * サイズ (サイズ1のcharはシフト不要)
    // 実行後: r{work_reg} = index * サイズ(バイトオフセット), r{work_reg+1} = シフト量(破棄可)
    if (shift > 0) {
        this->asm_file_ << "    mov fh r0 r" << (work_reg + 1) << " " << shift << "\n";
        this->asm_file_ << "    sll r" << work_reg << " r" << (work_reg + 1) << " r" << work_reg << "\n";
    }
    // アドレス = base + オフセット
    // 実行後: r{work_reg+1} = base番地 (val_reg・work_regを保護しつつr{work_reg+2}を作業用に使う)
    if (expr->children.size() == 2) {
        this->gen_member_array_base(expr, work_reg + 1, val_reg);
    } else {
        this->gen_array_base_addr(work_reg + 1, expr->sym);
    }
    // 実行後: r{work_reg} = base + オフセット = 書き込み先の実アドレス
    this->asm_file_ << "    add r" << work_reg << " r" << (work_reg + 1) << " r" << work_reg << "\n";
    // maskに従って書き込む (該当バイトのみ更新，他バイトはハードウェアが保持)
    this->asm_file_ << "    wm " << mask << " r" << work_reg << " r" << val_reg << "\n";
}
