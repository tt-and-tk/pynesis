#include <algorithm>
#include <iomanip>
#include <sstream>

#include "generator.hpp"

// 即値をアセンブリの表記にする
// 0x80000000以上の値は，10進で書くとアセンブラが出力するVerilogの10進定数(32ビット符号付き)の範囲を超えるため，16進(末尾h)で書く
static std::string imm_literal(long long value) {
    // intの範囲に収まる値(負の値を含む)は10進で書く
    if (value <= 0x7FFFFFFFLL) return std::to_string(value);
    std::ostringstream ss;   // 16進表記を組み立てるバッファ
    ss << std::uppercase << std::hex << (value & 0xFFFFFFFFLL) << 'h';
    return ss.str();
}

// 二項演算子の文字列を対応するアセンブリ命令に変換する (is_signedは符号付きで演算するか)
static std::string binop_mnemonic(const std::string &op, bool is_signed) {
    if (op == "+") return "add";
    if (op == "-") return "sub";
    if (op == "*") return "mul";
    if (op == "&") return "and";
    if (op == "|") return "or";
    if (op == "^") return "xor";
    if (op == "/") return is_signed ? "div" : "divu";     // 商 (余りは捨てる)
    if (op == "<<") return "sll";                          // 左シフト (空いたビットは符号によらず0で埋まる)
    if (op == ">>") return is_signed ? "sra" : "srl";      // 右シフト: 符号付きは算術シフト，符号なしは論理シフト
    // % は div/divu の4引数形式で別途生成する．比較・論理演算子は分岐の段階で対応する
    throw std::string("compiler error: unsupported binary operator '") + op + "'";
}

// 比較演算子かどうかを返す
static bool is_comparison(const std::string &op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

// 比較演算子の「否定」に対応するF系命令を返す (偽のとき分岐させるのに使う．is_signedは符号付きで比較するか)
// 大小比較の符号なし版は末尾にuを付けた命令になる (一致判定は符号によらないため同じ命令)
static std::string negated_branch(const std::string &op, bool is_signed) {
    const std::string u = is_signed ? "" : "u";     // 符号なしの大小比較に付ける接尾辞
    if (op == "==") return "ne";        // ==の否定は!=
    if (op == "!=") return "eq";        // !=の否定は==
    if (op == "<")  return "egt" + u;   // <の否定は>=
    if (op == ">")  return "elt" + u;   // >の否定は<=
    if (op == "<=") return "gt" + u;    // <=の否定は>
    if (op == ">=") return "lt" + u;    // >=の否定は<
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 比較演算子に「そのまま」対応するF系命令を返す (真のとき分岐させるのに使う．is_signedは符号付きで比較するか)
static std::string comparison_branch(const std::string &op, bool is_signed) {
    const std::string u = is_signed ? "" : "u";     // 符号なしの大小比較に付ける接尾辞
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<")  return "lt" + u;
    if (op == ">")  return "gt" + u;
    if (op == "<=") return "elt" + u;
    if (op == ">=") return "egt" + u;
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 比較・二項演算の式を符号付きで行うかを，両オペランドの型から返す
static bool is_signed_binop(const node_t *expr) {
    return is_signed_operation(expr->sval, expr->children[0]->type, expr->children[1]->type);
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

// 代入先の番地が実行時に決まる(配列要素または構造体配列要素のメンバである)かどうかを返す
static bool has_runtime_addr(const node_t *target) {
    // 配列要素そのもの，または基底が配列要素であるメンバアクセスなら実行時に番地が決まる
    return target->kind == ND_ARRAY_ACCESS
        || (target->kind == ND_MEMBER_ACCESS && target->children[0]->kind == ND_ARRAY_ACCESS);
}

// コンストラクタ: AST・シンボルテーブル・パラメータシンボル表・構造体定義表・
// 関数ごとのローカル変数領域の大きさ・呼び出しグラフ・グローバル領域の大きさ・出力先を受け取る
Generator::Generator(node_t *root, const std::map<std::string, const symbol_t *> &symbols,
                     const std::map<std::string, std::vector<const symbol_t *>> &func_params,
                     const std::map<std::string, struct_def_t> &struct_defs,
                     const std::map<std::string, int> &func_local_sizes,
                     const std::map<std::string, std::set<std::string>> &call_graph,
                     int global_size, std::ofstream &asm_file)
    : root_(root), symbols_(symbols), func_params_(func_params), struct_defs_(struct_defs),
      func_local_sizes_(func_local_sizes), call_graph_(call_graph), global_size_(global_size),
      asm_file_(asm_file), out_(&asm_file) {}

// コード生成を実行して .pt に書き出す
void Generator::operator()() {
    this->gen_program();
    // 全関数のフレームの大きさが確定したので，メモリ容量に収まるか検査する
    this->check_memory_usage();
}

// プログラム全体を生成する
// .global宣言で全関数名を列挙し，main関数を先頭に各関数を出力する
void Generator::gen_program() {
    // .global宣言: 子を走査して全関数名を集める (アセンブリは定義前に全関数の宣言が必要)
    bool first = true;
    (*this->out_) << ".global ";
    for (node_t *child : this->root_->children) {
        if (child->kind != ND_FUNC_DEF) continue;   // 関数定義のみ対象 (グローバル変数は除く)
        if (!first) (*this->out_) << ", ";
        (*this->out_) << child->sval;
        first = false;
    }
    (*this->out_) << "\n";

    // main関数を先頭に出力する (アセンブリはmainを一番最初に書く必要がある)
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval == "main") {
            (*this->out_) << "\n";
            this->gen_func(child);
            break;
        }
    }

    // main以外の関数を出力する
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval != "main") {
            (*this->out_) << "\n";
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
        this->gen_string_init(lit->sym, lit->sval);
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
// 関数ラベルを出力し，フレームを確保してから本体を生成する
// フレームの大きさはレジスタ退避領域の大きさに依存し，それは本体を生成してみないと分からないため，
// 出力を捨てる下見の生成で退避するレジスタを数えてから，確定した大きさで本体を生成し直す
// (退避するレジスタはフレームの大きさに依存しないため，下見の結果は本番の生成でもそのまま通用する)
void Generator::gen_func(node_t *func) {
    this->local_size_ = this->func_local_sizes_.at(func->sval);
    this->param_size_ = static_cast<int>(this->func_params_.at(func->sval).size()) * 4;

    // 下見: 退避に使う最大のレジスタ番号を数える (出力は捨て，ラベルの連番は生成し直す前に巻き戻す)
    const int label_count_before = this->label_count_;   // 下見を始める前のラベルの連番
    std::ostringstream discarded;                        // 下見の出力を捨てる先
    this->out_ = &discarded;
    this->max_spill_reg_ = -1;
    this->spill_size_ = MAX_REG * 4;   // 下見の間は最大の大きさを仮に置く (数えた結果には影響しない)
    this->gen_func_body(func);
    this->out_ = &this->asm_file_;
    this->label_count_ = label_count_before;

    // 本番: 数えたレジスタ番号から退避領域の大きさを確定させ，フレームを確保して本体を生成する
    this->spill_size_ = (this->max_spill_reg_ + 1) * 4;
    this->func_frame_sizes_[func->sval] = this->local_size_ + this->spill_size_ + this->param_size_;
    (*this->out_) << func->sval << ":\n";
    this->gen_frame_alloc(false);
    this->gen_func_body(func);
}

// 関数の本体と，末尾から関数を抜ける場合の復帰を生成する
void Generator::gen_func_body(node_t *func) {
    // mainの先頭でグローバル変数を初期化する (mainが最初に実行されるため)
    if (func->sval == "main") {
        this->gen_global_inits();
    }
    this->gen_block(func->children.back());   // 本体ブロック(最後の子)の文を生成する
    // 関数末尾のret (全関数にretが1つ以上必要)．return文を通らずに終端へ達した場合の復帰でもある
    this->gen_frame_alloc(true);
    (*this->out_) << "    ret\n";
}

// フレームぶんSPを下げて領域を確保する命令(is_release==false)，またはSPを戻して解放する命令
// (is_release==true)を出力する．即値を直接加減算する命令がないため，一度レジスタに載せてから計算する
// (作業用のr0は，関数の開始直後と復帰の直前であり，どちらも値を保持していないため自由に使える)
void Generator::gen_frame_alloc(bool is_release) {
    const int frame_size = this->local_size_ + this->spill_size_ + this->param_size_;   // フレームのバイト数
    // フレームを持たない関数の場合 (SPを動かす必要がないため，これ以上何もしない)
    if (frame_size == 0) return;

    (*this->out_) << "    mov fh r0 r0 " << frame_size << "\n";
    (*this->out_) << "    " << (is_release ? "add" : "sub")
                  << " " << SP_REGISTER << " r0 " << SP_REGISTER << "\n";
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
            (*this->out_) << "    jmp " << this->break_labels_.back() << "\n";
            break;
        // continue文: 最内ループの継続先へ飛ぶ
        case ND_CONTINUE:
            (*this->out_) << "    jmp " << this->continue_labels_.back() << "\n";
            break;
        // return文: 戻り値があればRAX(r30)に書き込み，フレームを解放してから復帰する
        case ND_RETURN:
            if (!stmt->children.empty()) {
                this->gen_expr(stmt->children[0], 0);                  // 戻り値の式 → r0
                (*this->out_) << "    mov fh r0 " << RAX_REGISTER << "\n";  // r0 → RAX
            }
            this->gen_frame_alloc(true);
            (*this->out_) << "    ret\n";
            break;
        default:
            break;
    }
}

// 変数宣言を生成する
// 初期化子があれば，初期値を変数の番地へ書き込むコードを生成する
void Generator::gen_var_decl(node_t *decl) {
    // const変数はメモリを持たず，参照箇所へ値が埋め込み済みのため何も出力しない
    if (decl->type.is_const) return;

    // 配列宣言: 文字列リテラルによる初期化のみ対応 (サイズ指定のみの宣言はスキップ)
    if (decl->type.is_array) {
        if (!decl->children.empty() && decl->children[0]->kind == ND_STRING_LIT) {
            this->gen_string_init(decl->sym, decl->children[0]->sval);
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
// 4文字ずつ1ワードにパックして書き込む (末尾にヌル終端を含む)
// グローバルの配列は絶対番地へwmで書き込み，フレーム上の配列はSPからの相対位置へwmrで書き込む
// (作業用にr0(書き込む値)を使う．文の単位で生成されるため，どちらのレジスタも自由に使える)
void Generator::gen_string_init(const symbol_t *sym, const std::string &str) {
    const bool is_global = sym->location == LOC_GLOBAL;   // 絶対番地で書き込めるか
    // 書き込み先の位置(グローバルは絶対番地，フレーム上はフレーム内オフセット)
    const int base = is_global ? sym->address : this->frame_offset(sym);

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
        // ワードをメモリに書き込む (wワード目は先頭から w*4 バイト目の位置)
        (*this->out_) << "    mov fh r0 r0 " << imm_literal(word) << "\n";
        if (is_global) {
            (*this->out_) << "    wm fh r0 r0 " << (base + w * 4) << "\n";
        } else {
            (*this->out_) << "    wmr fh " << SP_REGISTER << " r0 " << (base + w * 4) << "\n";
        }
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
    (*this->out_) << "    mov fh r0 r" << reg << " 0\n";                        // r{reg}   = インデックス(0)
    (*this->out_) << "    mov fh r0 r" << (reg + 3) << " " << sym->type.array_size << "\n";  // r{reg+3} = 配列サイズ(打ち切り境界)
    (*this->out_) << "    mov fh r0 r" << (reg + 4) << " 0\n";                  // r{reg+4} = 0 (ヌル終端比較用)
    (*this->out_) << "    mov fh r0 r" << (reg + 5) << " 1\n";                  // r{reg+5} = 1 (インデックス加算用)

    (*this->out_) << loop << ":\n";
    // 配列サイズに達したら打ち切る (ヌル終端がなくても無限ループ・範囲外読み出しを防ぐ)
    (*this->out_) << "    egt r" << reg << " r" << (reg + 3) << " " << end << "\n";
    // r{reg+2} = mem[base + index] (1バイト)
    (*this->out_) << "    add r" << (reg + 1) << " r" << reg << " r" << (reg + 2) << "\n";
    (*this->out_) << "    rm 1h r" << (reg + 2) << " r" << (reg + 2) << "\n";
    // ヌル終端なら終了
    (*this->out_) << "    eq r" << (reg + 2) << " r" << (reg + 4) << " " << end << "\n";
    (*this->out_) << "    print r" << (reg + 2) << "\n";
    (*this->out_) << "    add r" << reg << " r" << (reg + 5) << " r" << reg << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << end << ":\n";
}

// 標準入力を改行(\n=10)まで読み込み，char配列へヌル終端付きで格納するループを生成する
// 先頭の改行はすべて読み飛ばす．配列サイズ-1文字を超えたら追加のscanを行わず打ち切る
// (超過分は次にscanを実行したときに読み込まれる．そのためのscanが1回多く消費されることはない)
// DEL(0x7F，バックスペース)を受け取った場合は，直前に格納した1文字分インデックスを戻す(先頭では何もしない)
// レジスタ使用: r{reg}=読み込んだ文字, r{reg+1}=インデックス, r{reg+2}=ベースアドレス(不変), r{reg+3}=アドレス(作業用),
//              r{reg+4}='\n'(不変), r{reg+5}=配列サイズ-1(格納できる最大文字数，不変), r{reg+6}=1(インデックス加算用，不変),
//              r{reg+7}=0(ヌル終端書き込み用兼インデックス0判定用，不変), r{reg+8}=DEL(0x7F，不変)
void Generator::gen_scan_line(const symbol_t *sym, int reg) {
    if (reg + 8 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string skip_loop = this->new_label();
    const std::string skip_end = this->new_label();
    const std::string read_loop = this->new_label();
    const std::string del_branch = this->new_label();
    const std::string scan_next = this->new_label();
    const std::string read_end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(reg + 2, sym);                                              // r{reg+2} = ベースアドレス
    (*this->out_) << "    mov fh r0 r" << (reg + 4) << " 10\n";                       // r{reg+4} = '\n'
    (*this->out_) << "    mov fh r0 r" << (reg + 5) << " " << (sym->type.array_size - 1) << "\n";  // r{reg+5} = 配列サイズ-1
    (*this->out_) << "    mov fh r0 r" << (reg + 6) << " 1\n";                        // r{reg+6} = 1
    (*this->out_) << "    mov fh r0 r" << (reg + 7) << " 0\n";                        // r{reg+7} = 0
    (*this->out_) << "    mov fh r0 r" << (reg + 8) << " 7Fh\n";                      // r{reg+8} = DEL(0x7F)

    // 先頭の改行はすべて読み飛ばす
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << skip_loop << ":\n";
    (*this->out_) << "    ne r" << reg << " r" << (reg + 4) << " " << skip_end << "\n";  // 改行以外ならスキップ終了
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << "    jmp " << skip_loop << "\n";
    (*this->out_) << skip_end << ":\n";

    // 改行が来るまで1文字ずつ配列へ格納する
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";                        // r{reg+1} = インデックス(0)
    (*this->out_) << read_loop << ":\n";
    (*this->out_) << "    eq r" << reg << " r" << (reg + 4) << " " << read_end << "\n";  // 改行なら終了
    (*this->out_) << "    eq r" << reg << " r" << (reg + 8) << " " << del_branch << "\n";  // DELならバックスペース処理へ
    (*this->out_) << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    (*this->out_) << "    wm 1h r" << (reg + 3) << " r" << reg << "\n";                // buf[index] = 文字
    (*this->out_) << "    add r" << (reg + 1) << " r" << (reg + 6) << " r" << (reg + 1) << "\n";  // index += 1
    // 配列サイズ上限に達したら，これ以上scanせずに打ち切る (残りは次回のscanで読む)
    (*this->out_) << "    egt r" << (reg + 1) << " r" << (reg + 5) << " " << read_end << "\n";
    (*this->out_) << "    jmp " << scan_next << "\n";
    // バックスペース: 先頭(インデックス0)なら取り消す文字が無いので何もしない
    (*this->out_) << del_branch << ":\n";
    (*this->out_) << "    eq r" << (reg + 1) << " r" << (reg + 7) << " " << scan_next << "\n";
    (*this->out_) << "    sub r" << (reg + 1) << " r" << (reg + 6) << " r" << (reg + 1) << "\n";  // index -= 1
    (*this->out_) << scan_next << ":\n";
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << "    jmp " << read_loop << "\n";
    (*this->out_) << read_end << ":\n";

    // ヌル終端を書き込む
    (*this->out_) << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    (*this->out_) << "    wm 1h r" << (reg + 3) << " r" << (reg + 7) << "\n";           // buf[index] = 0
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

    // 各レジスタの役割に名前を付ける(この関数内では役割の使い回しをしない)
    const int r_result = reg;          // 比較結果(1=一致，0=不一致)
    const int r_index = reg + 1;       // インデックス
    const int r_base_a = reg + 2;      // ベースアドレスA(不変)
    const int r_base_b = reg + 3;      // ベースアドレスB(不変)
    const int r_limit = reg + 4;       // 打ち切り境界(不変)
    const int r_addr = reg + 5;        // アドレス(作業用，A/B共用)
    const int r_char_a = reg + 6;      // 配列Aから読んだ文字(作業用)
    const int r_char_b = reg + 7;      // 配列Bから読んだ文字(作業用)
    const int r_one = reg + 8;         // 1(インデックス加算用，不変)
    const int r_zero = reg + 9;        // 0(ヌル終端比較用，不変)

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(r_base_a, sym_a);                          // ベースアドレスAを取得する
    this->gen_array_base_addr(r_base_b, sym_b);                          // ベースアドレスBを取得する
    (*this->out_) << "    mov fh r0 r" << r_index << " 0\n";           // インデックスを0で初期化する
    (*this->out_) << "    mov fh r0 r" << r_limit << " " << size_limit << "\n";  // 打ち切り境界を設定する
    (*this->out_) << "    mov fh r0 r" << r_one << " 1\n";             // インデックス加算用に1を格納する
    (*this->out_) << "    mov fh r0 r" << r_zero << " 0\n";            // ヌル終端比較用に0を格納する

    (*this->out_) << loop << ":\n";
    // インデックスが打ち切り境界を超えた場合，ヌル終端が見つからないまま両配列の宣言サイズに達したとみなし，
    // mismatch(不一致確定)へジャンプする
    (*this->out_) << "    egt r" << r_index << " r" << r_limit << " " << mismatch << "\n";
    // 配列Aの現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_a << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char_a << "\n";
    // 配列Bの現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_b << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char_b << "\n";
    // 読み込んだ文字が異なる場合，不一致が確定したのでmismatchへジャンプする
    (*this->out_) << "    ne r" << r_char_a << " r" << r_char_b << " " << mismatch << "\n";
    // 両方ともヌル終端(文字コード0)であった場合，先頭からここまで全て一致したとみなし，match(一致確定)へジャンプする
    (*this->out_) << "    eq r" << r_char_a << " r" << r_zero << " " << match << "\n";
    // 次の文字を比較するため，インデックスを1つ進めてloopの先頭へ戻る
    (*this->out_) << "    add r" << r_index << " r" << r_one << " r" << r_index << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << match << ":\n";
    // 比較結果を「一致」として格納し，end(終了処理)へジャンプする
    (*this->out_) << "    mov fh r0 r" << r_result << " 1\n";
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << mismatch << ":\n";
    // 比較結果を「不一致」として格納する
    (*this->out_) << "    mov fh r0 r" << r_result << " 0\n";
    (*this->out_) << end << ":\n";
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

    // 各レジスタの役割に名前を付ける(この関数内では役割の使い回しをしない)
    const int r_char = reg;            // コピー中の文字(作業用)
    const int r_index = reg + 1;       // インデックス
    const int r_base_dst = reg + 2;    // ベースアドレス(コピー先，不変)
    const int r_base_src = reg + 3;    // ベースアドレス(コピー元，不変)
    const int r_addr = reg + 4;        // アドレス(作業用)
    const int r_zero = reg + 5;        // 0(ヌル終端書き込み用，不変)
    const int r_limit = reg + 6;       // コピー先の配列サイズ-1(打ち切り境界，不変)
    const int r_one = reg + 7;         // 1(インデックス加算用，不変)

    // 必要な変数をレジスタに格納する
    this->gen_array_base_addr(r_base_dst, dst);                          // ベースアドレス(コピー先)を取得する
    this->gen_array_base_addr(r_base_src, src);                          // ベースアドレス(コピー元)を取得する
    (*this->out_) << "    mov fh r0 r" << r_index << " 0\n";           // インデックスを0で初期化する
    (*this->out_) << "    mov fh r0 r" << r_zero << " 0\n";            // ヌル終端書き込み用に0を格納する
    (*this->out_) << "    mov fh r0 r" << r_limit << " " << (dst->type.array_size - 1) << "\n";  // 打ち切り境界を設定する
    (*this->out_) << "    mov fh r0 r" << r_one << " 1\n";             // インデックス加算用に1を格納する

    (*this->out_) << loop << ":\n";
    // インデックスがコピー先の宣言サイズ-1文字を超えた場合，これ以上格納する余地がないので
    // truncate(打ち切り処理)へジャンプする
    (*this->out_) << "    egt r" << r_index << " r" << r_limit << " " << truncate << "\n";
    // コピー元の現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_src << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char << "\n";
    // 読み込んだ文字がヌル終端(文字コード0)であった場合，コピーすべき文字は終わったのでfinish(終端処理)へジャンプする
    (*this->out_) << "    eq r" << r_char << " r" << r_zero << " " << finish << "\n";
    // 読み込んだ文字をコピー先の現在インデックスへ書き込む
    (*this->out_) << "    add r" << r_base_dst << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_char << "\n";
    // 次の文字をコピーするため，インデックスを1つ進めてloopの先頭へ戻る
    (*this->out_) << "    add r" << r_index << " r" << r_one << " r" << r_index << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << truncate << ":\n";
    // コピー先の末尾(宣言サイズ-1文字目)にヌル終端を書き込み，end(終了処理)へジャンプする
    (*this->out_) << "    add r" << r_base_dst << " r" << r_limit << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_zero << "\n";
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << finish << ":\n";
    // コピー先の現在インデックスにヌル終端を書き込む
    (*this->out_) << "    add r" << r_base_dst << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_zero << "\n";
    (*this->out_) << end << ":\n";
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
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(cond->loc);
    }
    // 比較条件: 否定したF系で「偽のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, {reg});   // 右 → r{reg+1}
        (*this->out_) << "    " << negated_branch(cond->sval, is_signed_binop(cond))
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，0(偽)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                      // cond → r{reg}
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";    // r{reg+1} = 0
        (*this->out_) << "    eq r" << reg << " r" << (reg + 1) << " " << label << "\n";  // 0なら飛ぶ
    }
}

// 条件式condが真のとき，labelへ分岐する命令を出力する
// 評価にはr{reg}・r{reg+1}を使う
void Generator::gen_branch_if_true(node_t *cond, const std::string &label, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(cond->loc);
    }
    // 比較条件: そのままのF系で「真のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, {reg});   // 右 → r{reg+1}
        (*this->out_) << "    " << comparison_branch(cond->sval, is_signed_binop(cond))
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，0でない(真)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                      // cond → r{reg}
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";    // r{reg+1} = 0
        (*this->out_) << "    ne r" << reg << " r" << (reg + 1) << " " << label << "\n";  // 0以外なら飛ぶ
    }
}

// 比較演算を0/1の値としてr{reg}に生成する
// 左をr{reg}・右をr{reg+1}に評価し，比較が真なら1・偽なら0をr{reg}に置く
void Generator::gen_compare(node_t *expr, int reg) {
    const std::string t = this->new_label();      // 真の場合の飛び先
    const std::string end = this->new_label();
    this->gen_expr(expr->children[0], reg);        // 左 → r{reg}
    this->gen_expr_protecting(expr->children[1], reg + 1, {reg});    // 右 → r{reg+1}
    // 比較が真なら .Lt へ
    (*this->out_) << "    " << comparison_branch(expr->sval, is_signed_binop(expr))
                    << " r" << reg << " r" << (reg + 1) << " " << t << "\n";
    (*this->out_) << "    mov fh r0 r" << reg << " 0\n";   // 偽: r{reg} = 0
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << t << ":\n";
    (*this->out_) << "    mov fh r0 r" << reg << " 1\n";   // 真: r{reg} = 1
    (*this->out_) << end << ":\n";
}

// 論理 && / || を短絡評価し，結果(0/1)をr{reg}に生成する
// && は0で短絡(両方非0で1)，|| は非0で短絡(両方0で0)
void Generator::gen_logical(node_t *expr, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }
    const bool is_and = (expr->sval == "&&");
    const std::string shortcut = this->new_label();   // 短絡時の飛び先
    const std::string end = this->new_label();
    // 短絡判定のF系: && は「0なら短絡(eq)」, || は「0以外なら短絡(ne)」
    const std::string br = is_and ? "eq" : "ne";

    // 左を評価．短絡条件を満たせば右を評価する命令を飛ばして結果へ行く
    this->gen_expr(expr->children[0], reg);
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";
    (*this->out_) << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // 右を評価．こちらは飛ばす対象が無いので短絡ではなく，結果(0/1)を確定させるための判定
    this->gen_expr(expr->children[1], reg);
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";
    (*this->out_) << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // どちらも短絡しなかった場合の結果 (&&なら1, ||なら0)
    (*this->out_) << "    mov fh r0 r" << reg << " " << (is_and ? 1 : 0) << "\n";
    (*this->out_) << "    jmp " << end << "\n";
    // 短絡した場合の結果 (&&なら0, ||なら1)
    (*this->out_) << shortcut << ":\n";
    (*this->out_) << "    mov fh r0 r" << reg << " " << (is_and ? 0 : 1) << "\n";
    (*this->out_) << end << ":\n";
}

// 三項演算子 a ? b : c の結果をr{reg}に生成する
void Generator::gen_ternary(node_t *expr, int reg) {
    const std::string else_label = this->new_label();
    const std::string end = this->new_label();
    this->gen_branch_if_false(expr->children[0], else_label, reg);   // 条件が偽ならelse値へ
    this->gen_expr(expr->children[1], reg);                          // then値 → r{reg}
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << else_label << ":\n";
    this->gen_expr(expr->children[2], reg);                          // else値 → r{reg}
    (*this->out_) << end << ":\n";
}

// インクリメント/デクリメント (++/--) を生成する
// 対象は変数 (アナライザが読み書き可能を保証)．前置は増減後の新値・後置は増減前の旧値を式の値とする
void Generator::gen_incdec(node_t *expr, int reg, bool is_prefix) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }
    node_t *var = expr->children[0];                          // 対象変数 (ND_VAR)
    const std::string op = (expr->sval == "++") ? "+" : "-";  // ++→加算, --→減算

    // 現在値を読み，1を載せる
    this->gen_load(reg, var->sym, var->loc);                              // r{reg} = x
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 1\n";         // r{reg+1} = 1

    // 加減算は符号によって命令が変わらないため，符号付きかどうかは常に真として渡す
    if (is_prefix) {
        // 前置 ++x/--x : r{reg}を増減して書き戻す (新値がそのまま式の値として残る)
        this->gen_binop_instr(op, true, reg, reg, reg + 1);                 // r{reg} = x ± 1
        this->gen_store(reg, var->sym);                                     // x = r{reg}
    } else {
        // 後置 x++/x-- : 旧値をr{reg}に残したまま，新値をr{reg+1}で計算して書き戻す
        this->gen_binop_instr(op, true, reg + 1, reg, reg + 1);             // r{reg+1} = x ± 1
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
            throw std::string("compiler error: expression too complex (out of registers) at ")
                  + loc_to_string(expr->loc);
        }
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";                          // r{reg+1} = 0
        (*this->out_) << "    sub r" << (reg + 1) << " r" << reg << " r" << reg << "\n";    // r{reg} = 0 - x
    } else if (op == "~") {
        // ビット反転 : NOT命令 (not rs1 rd)
        (*this->out_) << "    not r" << reg << " r" << reg << "\n";                         // r{reg} = ~x
    } else if (op == "!") {
        // 論理否定 : x==0 なら1，それ以外は0 (比較と同じ0/1生成パターン，r{reg+1}を使うため上限(r15)を超えないことを確認する)
        if (reg + 1 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at ")
                  + loc_to_string(expr->loc);
        }
        const std::string t = this->new_label();      // 真(x==0)の飛び先
        const std::string end = this->new_label();
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";                          // r{reg+1} = 0
        (*this->out_) << "    eq r" << reg << " r" << (reg + 1) << " " << t << "\n";        // x==0 なら .Lt へ
        (*this->out_) << "    mov fh r0 r" << reg << " 0\n";   // x!=0: r{reg} = 0
        (*this->out_) << "    jmp " << end << "\n";
        (*this->out_) << t << ":\n";
        (*this->out_) << "    mov fh r0 r" << reg << " 1\n";   // x==0: r{reg} = 1
        (*this->out_) << end << ":\n";
    } else {
        throw std::string("compiler error: unsupported unary operator '") + op
              + "' at " + loc_to_string(expr->loc);
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
        (*this->out_) << end << ":\n";
    } else {
        // if (cond) then else else節 : 偽ならelseへ，thenの後はelseを飛ばす
        const std::string else_label = this->new_label();
        const std::string end = this->new_label();
        this->gen_branch_if_false(cond, else_label);
        this->gen_stmt(stmt->children[1]);
        (*this->out_) << "    jmp " << end << "\n";
        (*this->out_) << else_label << ":\n";
        this->gen_stmt(stmt->children[2]);
        (*this->out_) << end << ":\n";
    }
}

// while文を生成する
// children: [0]=条件, [1]=本体
void Generator::gen_while(node_t *stmt) {
    const std::string top = this->new_label();   // 条件判定の先頭 (continueの飛び先)
    const std::string end = this->new_label();   // ループ脱出先 (breakの飛び先)
    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(top);

    (*this->out_) << top << ":\n";
    this->gen_branch_if_false(stmt->children[0], end);   // 条件が偽なら脱出
    this->gen_stmt(stmt->children[1]);                   // 本体
    (*this->out_) << "    jmp " << top << "\n";        // 先頭(条件)へ戻る
    (*this->out_) << end << ":\n";

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

    (*this->out_) << top << ":\n";
    // 条件 (省略時は判定なし＝常にループ)
    if (cond != nullptr) this->gen_branch_if_false(cond, end);
    this->gen_stmt(body);                            // 本体
    (*this->out_) << cont << ":\n";                // continueはここ(更新部)へ来る
    if (update != nullptr) this->gen_stmt(update);   // 更新 (省略可)
    (*this->out_) << "    jmp " << top << "\n";    // 条件へ戻る
    (*this->out_) << end << ":\n";

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

    (*this->out_) << top << ":\n";
    this->gen_stmt(stmt->children[0]);             // 本体
    (*this->out_) << cont << ":\n";              // continueはここ(条件判定)へ来る
    this->gen_branch_if_true(stmt->children[1], top);   // 条件が真なら先頭へ戻る
    (*this->out_) << end << ":\n";

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
            (*this->out_) << "    mov fh r0 r1 " << imm_literal(c->ival) << "\n";    // r1 = case値
            (*this->out_) << "    eq r0 r1 " << label_of[c] << "\n";    // 一致ならそのcaseへ
        }
    }
    // どのcaseにも一致しなければ default へ (無ければ end へ)
    (*this->out_) << "    jmp " << (default_label.empty() ? end : default_label) << "\n";

    // 本体: case/defaultラベルを所定位置に置き，文を順に生成する (フォールスルーは自然に表現される)
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE || c->kind == ND_DEFAULT) {
            (*this->out_) << label_of[c] << ":\n";
        } else {
            this->gen_stmt(c);
        }
    }
    (*this->out_) << end << ":\n";

    this->break_labels_.pop_back();
}

// r{dst} = r{lhs} op r{rhs} となる演算命令を出力する (is_signedは符号付きで演算するか)
// 二項演算と複合代入で共用する (剰余だけはdiv/divuの4引数形式)
void Generator::gen_binop_instr(const std::string &op, bool is_signed, int dst, int lhs, int rhs) {
    // 剰余: div/divuは商をrdへ・余りをimmが指すレジスタ番地へ格納する
    // 商をr{rhs}に捨て，余りをr{dst}(番地dst)へ得る
    if (op == "%") {
        (*this->out_) << "    " << (is_signed ? "div" : "divu") << " r" << lhs << " r" << rhs
                        << " r" << rhs << " " << dst << "\n";
    } else {
        // それ以外は単一命令
        const std::string mn = binop_mnemonic(op, is_signed);     // 演算子→命令
        (*this->out_) << "    " << mn
                        << " r" << lhs << " r" << rhs << " r" << dst << "\n";
    }
}

// 変数の型に応じた，メモリの読み書きに使うmaskを返す
// (char/shortは型幅のバイトだけを対象にし，読み込みでは他バイトのゴミを混入させず，
//  書き込みでは桁あふれした上位ビットを書き込まない)
// 配列(配列パラメータを含む)は要素の値ではなく「先頭アドレス」を読み書きするため，
// 要素型によらず常にfh(全32ビット)になる (char配列のアドレスを1バイトに切り詰めてはいけない)
static const char *access_mask(const type_t &type) {
    if (type.is_array) return "fh";
    switch (type.base) {
        case BASE_CHAR:  return "1h";
        case BASE_SHORT: return "3h";
        case BASE_INT:   return "fh";
        default:
            throw std::string("compiler error: unsupported scalar type in memory access");
    }
}

// フレーム上の変数の，プロローグ直後のSPから数えたオフセットを返す
// ローカル変数領域はフレームの先頭にあるためオフセットがそのまま位置になり，
// パラメータ領域はローカル変数領域とレジスタ退避領域の後ろに続く
int Generator::frame_offset(const symbol_t *sym) const {
    if (sym->location == LOC_PARAM) {
        return this->local_size_ + this->spill_size_ + sym->address;
    }
    return sym->address;
}

// 変数の値をr{reg}へ読み込む
// 置き場所がレジスタ直結(LED等のI/Oレジスタ)ならmovのレジスタ間コピー，グローバル変数ならrm，
// フレーム上の変数(ローカル変数・パラメータ)ならSPからの相対位置を指定するrmrを使う
// 符号付きchar/shortは読み込み後に符号拡張する
// (レジスタ上の演算は型に関係なく常に32ビットで行うため，char/shortはintに昇格した状態で保持する．
//  読み込みはmaskで選ばなかった上位バイトを0で埋めるため，符号なしは読み込んだままでゼロ拡張になっている)
void Generator::gen_load(int reg, const symbol_t *sym, const loc_t &loc) {
    if (sym->location == LOC_REGISTER) {
        // mov rs1=番地, rd=r{reg} : r{reg} = register[番地] (即値を付けないとレジスタ間コピーになる)
        (*this->out_) << "    mov fh r" << sym->address << " r" << reg << "\n";
        return;
    }
    const char *mask = access_mask(sym->type);   // 型幅に応じたmask
    if (sym->location == LOC_GLOBAL) {
        // rm: メモリ絶対番地からr{reg}へ読み込む (即値アドレス指定のためrs1のr0は無視される)
        (*this->out_) << "    rm " << mask << " r0 r" << reg << " " << sym->address << "\n";
    } else {
        // rmr: SPにフレーム内オフセットを足した番地からr{reg}へ読み込む
        (*this->out_) << "    rmr " << mask << " " << SP_REGISTER << " r" << reg
                      << " " << this->frame_offset(sym) << "\n";
    }
    // 符号付きchar/shortの場合 (値の上位を符号ビットで埋めてintの値にする)
    if (!sym->type.is_array && sym->type.is_signed) {
        if (sym->type.base == BASE_CHAR)  this->gen_sign_extend(reg, 8, reg + 1, loc);
        if (sym->type.base == BASE_SHORT) this->gen_sign_extend(reg, 16, reg + 1, loc);
    }
}

// r{reg}の値を変数へ書き込む
// 置き場所がレジスタ直結(LED等のI/Oレジスタ)ならmovのレジスタ間コピー，グローバル変数ならwm，
// フレーム上の変数(ローカル変数・パラメータ)ならSPからの相対位置を指定するwmrを使う
void Generator::gen_store(int reg, const symbol_t *sym) {
    if (sym->location == LOC_REGISTER) {
        // mov rs1=r{reg}, rd=番地 : register[番地] = r{reg}
        (*this->out_) << "    mov fh r" << reg << " r" << sym->address << "\n";
        return;
    }
    const char *mask = access_mask(sym->type);   // 型幅に応じたmask
    if (sym->location == LOC_GLOBAL) {
        // wm: r{reg}をメモリ絶対番地へ書き込む (即値アドレス指定のためrs1のr0は無視される)
        (*this->out_) << "    wm " << mask << " r0 r" << reg << " " << sym->address << "\n";
    } else {
        // wmr: SPにフレーム内オフセットを足した番地へr{reg}を書き込む
        (*this->out_) << "    wmr " << mask << " " << SP_REGISTER << " r" << reg
                      << " " << this->frame_offset(sym) << "\n";
    }
}

// r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング，結果は同じレジスタに上書き)
// gen_loadのメモリ変数分岐と同じマスク・符号拡張(符号付きのみ)の手順を，即値アドレスではなくレジスタが持つ実行時アドレスに適用する．
// 符号拡張の作業用レジスタは呼び出し側が指定する(読み込み後も値を保持したいレジスタを避けられるようにするため)
void Generator::gen_load_indirect(int reg, const type_t &type, int work_reg, const loc_t &loc) {
    switch (type.base) {
        // char: 下位1バイトを読み込み，符号付きなら8ビット値として符号拡張する
        case BASE_CHAR:
            (*this->out_) << "    rm 1h r" << reg << " r" << reg << "\n";
            if (type.is_signed) this->gen_sign_extend(reg, 8, work_reg, loc);
            break;
        // short: 下位2バイトを読み込み，符号付きなら16ビット値として符号拡張する
        case BASE_SHORT:
            (*this->out_) << "    rm 3h r" << reg << " r" << reg << "\n";
            if (type.is_signed) this->gen_sign_extend(reg, 16, work_reg, loc);
            break;
        // int: 4バイトすべてを読み込む (符号拡張は不要)
        case BASE_INT:
            (*this->out_) << "    rm fh r" << reg << " r" << reg << "\n";
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
    (*this->out_) << "    wm " << mask << " r" << addr_reg << " r" << val_reg << "\n";
}

// r{reg}の下位bitsビットを符号として32ビットへ符号拡張する(シフト量の保持にr{work_reg}を使う)
void Generator::gen_sign_extend(int reg, int bits, int work_reg, const loc_t &loc) {
    // 作業用レジスタが上限(r15)を超えないことを確認する (r16以降はSP等の汎用でないレジスタのため)
    if (work_reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(loc);
    }
    const int shift = 32 - bits;   // 値の最上位ビットをレジスタのMSBへ運ぶシフト量
    // シフト量を保存しておく
    (*this->out_) << "    mov fh r0 r" << work_reg << " " << shift << "\n";
    // 最上位ビットをMSBにシフトする
    (*this->out_) << "    sll r" << reg << " r" << work_reg << " r" << reg << "\n";
    // 算術シフトして，実際の値が入っているよりも上位のビットを符号ビットで埋める
    (*this->out_) << "    sra r" << reg << " r" << work_reg << " r" << reg << "\n";
}

// 式を評価し結果を指定レジスタに残す．評価対象の式が関数呼び出しを含む場合，
// 呼び出し先はr0から使い直すため，別に指定したレジスタ(複数可)の値をフレーム上の退避領域へ
// 退避してから評価し，評価後に復元する
// (呼び出し元が評価済みの値(二項演算の左辺・代入先のアドレス等)をレジスタに置いたまま，後続の式で関数を呼ぶ場面で使う)
// 退避先を呼び出しごとのフレーム内に置くことで，呼び出し先が同じレジスタを退避しても
// 呼び出し元の退避値を壊さない(固定番地の退避領域を共有すると，再帰では必ず壊れる)
void Generator::gen_expr_protecting(node_t *expr, int reg, const std::vector<int> &protect_regs) {
    // 関数呼び出しを含まない式はreg以上のレジスタしか使わず保護対象を壊さないため，退避せずに評価する
    if (!contains_call(expr)) {
        this->gen_expr(expr, reg);
        return;
    }
    // 保護するレジスタの値を，レジスタ番号ごとに決まった退避枠へ書き出す
    for (const int protect_reg : protect_regs) {
        // 退避枠の大きさを決めるため，退避したレジスタのうち最大の番号を控える
        if (protect_reg > this->max_spill_reg_) this->max_spill_reg_ = protect_reg;
        const int offset = this->local_size_ + protect_reg * 4;   // r{protect_reg}の退避枠のフレーム内オフセット
        (*this->out_) << "    wmr fh " << SP_REGISTER << " r" << protect_reg << " " << offset << "\n";
    }
    // 式を評価する (呼び出し先がレジスタを使い直すため，保護するレジスタの値はここで壊れうる)
    this->gen_expr(expr, reg);
    // 退避枠から値を読み戻し，保護するレジスタを評価前の値に戻す
    for (const int protect_reg : protect_regs) {
        const int offset = this->local_size_ + protect_reg * 4;   // r{protect_reg}の退避枠のフレーム内オフセット
        (*this->out_) << "    rmr fh " << SP_REGISTER << " r" << protect_reg << " " << offset << "\n";
    }
}

// 式を評価し，結果をr{reg}に残す
// reg以上のレジスタを作業用に使うレジスタスタック方式 (二項演算は左をr{reg}・右をr{reg+1}に評価して畳む)
void Generator::gen_expr(node_t *expr, int reg) {
    // レジスタは16本(r0〜r15)．深い式で枯渇したらエラーにする
    if (reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }

    switch (expr->kind) {
        // リテラル: 即値をr{reg}に載せる
        // sizeofは意味解析でコンパイル時に値(ival)が確定済みのため，リテラルと同じ即値ロードで済む
        case ND_INT_LIT:
        case ND_CHAR_LIT:
        case ND_SIZEOF:
            (*this->out_) << "    mov fh r0 r" << reg << " " << imm_literal(expr->ival) << "\n";
            break;

        // 文字列リテラル: 配列名と同様，先頭の番地(コンパイル時確定の即値)をr{reg}に載せる
        // データ自体はgen_global_initsで1回だけ書き込み済み
        // 現状は関数の引数としてしか使用されないので，先頭アドレスだけ保存すればいい
        case ND_STRING_LIT:
            (*this->out_) << "    mov fh r0 r" << reg << " " << expr->sym->address << "\n";
            break;

        // 変数参照: 変数の値をr{reg}へ読み込む
        case ND_VAR:
            this->gen_load(reg, expr->sym, expr->loc);
            break;

        // 構造体メンバ参照:
        // 単一の構造体変数のメンバは，意味解析が「構造体変数の番地+メンバのオフセット」を
        // 1つのシンボルに合成済みのため，通常の変数参照と同じ経路で読み込める．
        // 構造体配列要素のメンバ(arr[i].member)は番地が実行時に決まるため，
        // アドレスを計算してからレジスタ間接で読み込む
        case ND_MEMBER_ACCESS:
            if (expr->children[0]->kind == ND_ARRAY_ACCESS) {
                // メンバの実アドレスをr{reg}に求める
                this->gen_struct_array_member_addr(expr, reg);
                // そのアドレスからメンバの値をr{reg}へ読み込む (符号拡張の作業用にr{reg+1}を使う)
                this->gen_load_indirect(reg, expr->type, reg + 1, expr->loc);
            } else {
                this->gen_load(reg, expr->sym, expr->loc);
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
                // 左辺をr{reg}に評価する
                this->gen_expr(expr->children[0], reg);
                // 右辺をr{reg+1}に評価する (関数呼び出しを含む場合はr{reg}の左辺の値を保護する)
                this->gen_expr_protecting(expr->children[1], reg + 1, {reg});
                // 左辺と右辺を演算子で畳み，結果をr{reg}に置く
                this->gen_binop_instr(expr->sval, is_signed_binop(expr), reg, reg, reg + 1);   // r{reg} = r{reg} op r{reg+1}
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

        // 関数呼び出し: 各引数を評価して現在のSPより下へ書き込んでからCALLする
        // 書き込み先は，呼び出し先が自分のフレームを確保したときにパラメータ領域になる位置であり，
        // 戻り先アドレスの1つ下から引数の宣言順に並ぶ(i番目の引数はSP-4-4n+4i番地)．
        // 評価にr{reg}以降を使うのは，呼び出し元がr{reg}未満のレジスタに置いている生存値
        // (二項演算の左辺等)を破壊しないため．callでレジスタは揮発するが，
        // 呼び出し前後で生きた値はメモリにあるため問題ない
        case ND_CALL: {
            const auto &params = this->func_params_.at(expr->sval);
            const int arg_count = static_cast<int>(expr->children.size());   // 引数の個数
            // 引数を書き込む位置は呼び出し先のフレーム全体より下にあり，呼び出し先が引数を
            // 書き込む位置と重なる．このため，関数呼び出しを含む引数がある場合は，その引数までを
            // レジスタに保持したまま評価し，内側の呼び出しがすべて終わってから書き込む
            // (先に書き込むと，内側の呼び出しが同じ位置へ自分の引数を書いて壊してしまう)．
            // 最後に関数呼び出しを含む引数より後ろの引数は，以降に呼び出しがないため評価のたびに書き込む
            int held_count = 0;   // 評価した値をレジスタに保持したままにする引数の個数
            for (int i = 0; i < arg_count; i++) {
                if (contains_call(expr->children[i])) held_count = i + 1;
            }
            // 保持する引数の個数だけレジスタが同時に必要になる
            if (held_count > 0 && reg + held_count - 1 >= MAX_REG) {
                throw std::string("compiler error: expression too complex (out of registers) at ")
                      + loc_to_string(expr->loc);
            }

            for (int i = 0; i < arg_count; i++) {
                node_t *arg = expr->children[i];
                // 保持する引数はそれぞれ別のレジスタへ，書き込む引数は共通のr{reg}へ評価する
                const int arg_reg = (i < held_count) ? reg + i : reg;   // この引数を評価するレジスタ
                if (params[i]->type.is_array) {
                    // 配列引数: ベースアドレスをロードする
                    this->gen_array_base_addr(arg_reg, arg->sym);
                } else {
                    // スカラー引数: 式を評価する (評価済みの引数を保持している間は，それらを保護する)
                    std::vector<int> protect_regs;   // 評価中に値を保護するレジスタ
                    for (int held = 0; held < i && held < held_count; held++) {
                        protect_regs.push_back(reg + held);
                    }
                    this->gen_expr_protecting(arg, arg_reg, protect_regs);
                }
                // 保持する引数は，すべての評価が終わってからまとめて書き込む
                if (i >= held_count) {
                    (*this->out_) << "    wmr " << access_mask(params[i]->type) << " " << SP_REGISTER
                                  << " r" << arg_reg << " " << (-4 - arg_count * 4 + i * 4) << "\n";
                }
            }
            // 保持していた引数をパラメータ領域になる位置へ書き込む
            for (int i = 0; i < held_count; i++) {
                (*this->out_) << "    wmr " << access_mask(params[i]->type) << " " << SP_REGISTER
                              << " r" << (reg + i) << " " << (-4 - arg_count * 4 + i * 4) << "\n";
            }

            (*this->out_) << "    call " << expr->sval << "\n";
            // 非void関数はRAX(r30)から戻り値を取り出す
            if (expr->type.base != BASE_VOID) {
                (*this->out_) << "    mov fh " << RAX_REGISTER << " r" << reg << "\n";
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
            node_t *lhs = expr->children[0];   // 代入先(左辺)
            // 番地が実行時に決まる代入先の場合
            if (has_runtime_addr(lhs)) {
                // 配列要素・構造体配列要素のメンバへの代入: 番地が実行時計算のため，アドレスを求めてから読み書きする．
                // 式の評価はreg以上のレジスタしか使わない規約(レジスタスタック方式)のため，
                // アドレスは右辺(または現在値の読み出し)より後ろのレジスタ(r{reg+1})に置く
                // (先にr{reg+1}へアドレスを置いてしまうと，reg起点で評価する式が
                //  自身の作業用としてr{reg+1}を使い，アドレスを上書きしてしまう)．
                // レジスタ使用: r{reg}=右辺値/現在値，r{reg+1}=アドレス(アドレス計算・右辺の評価にr{reg+2}以降も使う)
                // 作業用のr{reg+2}が上限(r15)を超えないことを確認する
                if (reg + 2 >= MAX_REG) {
                    throw std::string("compiler error: expression too complex (out of registers) at ")
                          + loc_to_string(expr->loc);
                }
                if (expr->sval == "=") {
                    // 単純代入: 右辺を先にr{reg}へ評価してから，アドレスをr{reg+1}へ求める(r{reg}を保護)
                    // 右辺の値をr{reg}に求める
                    this->gen_expr(expr->children[1], reg);
                    // 代入先のアドレスをr{reg+1}に求める (添字の関数呼び出しからr{reg}の右辺の値を保護する)
                    this->gen_runtime_addr(lhs, reg + 1, {reg});
                } else {
                    // 複合代入 x op= e : アドレスをr{reg+1}へ求め，現在値をr{reg}へ読む
                    // (char/shortの符号拡張は，r{reg+1}のアドレスを壊さないようr{reg+2}を作業用に使う)．
                    // 右辺の評価が関数呼び出しを含む場合，呼び出し先はr0から使い直すため
                    // r{reg}(現在値)・r{reg+1}(アドレス)の両方を保護して評価する
                    // 代入先のアドレスをr{reg+1}に求める
                    this->gen_runtime_addr(lhs, reg + 1);
                    // アドレスをr{reg}へ複製する (読み込みは結果を読み込み元と同じレジスタに上書きするため)
                    (*this->out_) << "    mov fh r" << (reg + 1) << " r" << reg << "\n";  // r{reg} = アドレス
                    // 代入先の現在値をr{reg}へ読み込む
                    this->gen_load_indirect(reg, lhs->type, reg + 2, lhs->loc);             // r{reg} = 現在値
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);    // "+=" → "+"
                    const bool is_signed = is_signed_operation(op, lhs->type, expr->children[1]->type);   // 符号付きで演算するか
                    // 右辺をr{reg+2}に評価する
                    this->gen_expr_protecting(expr->children[1], reg + 2, {reg, reg + 1}); // 右辺 → r{reg+2}
                    // 現在値と右辺を演算子で畳み，結果をr{reg}に置く
                    this->gen_binop_instr(op, is_signed, reg, reg, reg + 2);
                }
                // r{reg}の値を，r{reg+1}のアドレスへ型に応じたマスクで書き込む (代入式の値もr{reg}に残る)
                this->gen_store_indirect(reg + 1, reg, lhs->type);
            } else {
                // スカラー変数への代入
                if (expr->sval == "=") {
                    // 単純代入: 右辺をr{reg}に評価する
                    this->gen_expr(expr->children[1], reg);
                } else {
                    // 複合代入 x op= e : 左辺の現在値をr{reg}・右辺をr{reg+1}に評価し，opで畳む
                    // 左辺の現在値をr{reg}に読み込む
                    this->gen_load(reg, lhs->sym, lhs->loc);
                    // 右辺をr{reg+1}に評価する (関数呼び出しを含む場合はr{reg}の現在値を保護する)
                    this->gen_expr_protecting(expr->children[1], reg + 1, {reg});
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);   // "+=" → "+"
                    const bool is_signed = is_signed_operation(op, lhs->type, expr->children[1]->type);   // 符号付きで演算するか
                    // 現在値と右辺を演算子で畳み，結果をr{reg}に置く
                    this->gen_binop_instr(op, is_signed, reg, reg, reg + 1);
                }
                // 変数へ書き込む (代入式の値もr{reg}に残る)
                this->gen_store(reg, lhs->sym);
            }
            break;
        }

        // 配列要素アクセス(読み出し): インデックスからメモリアドレスを計算し，レジスタ間接で読み込む
        case ND_ARRAY_ACCESS:
            // 要素の実アドレスをr{reg}に求める
            this->gen_array_elem_addr(expr, reg);
            // そのアドレスから要素の値をr{reg}へ読み込む (符号拡張の作業用にr{reg+1}を使う)
            this->gen_load_indirect(reg, expr->type, reg + 1, expr->loc);
            break;

        default:
            throw std::string("compiler error: unsupported expression in code generation at ")
                  + loc_to_string(expr->loc);
    }
}

// 配列の先頭アドレスをr{reg}に載せる
// 配列パラメータ(array_size==0)は，呼び出し元が書き込んだ先頭アドレスをそのスロットから読み出す．
// 直接配列のうちグローバルはコンパイル時にアドレスが確定するので即値ロード，
// フレーム上のものは実行時のSPにフレーム内オフセットを足して求める
void Generator::gen_array_base_addr(int reg, const symbol_t *sym) {
    // 配列パラメータの場合 (パラメータ領域のスロットが先頭アドレスそのものを保持している)
    if (sym->type.array_size == 0) {
        (*this->out_) << "    rmr fh " << SP_REGISTER << " r" << reg
                      << " " << this->frame_offset(sym) << "\n";
        return;
    }
    if (sym->location == LOC_GLOBAL) {
        (*this->out_) << "    mov fh r0 r" << reg << " " << sym->address << "\n";
        return;
    }
    (*this->out_) << "    mov fh r0 r" << reg << " " << this->frame_offset(sym) << "\n";
    (*this->out_) << "    add " << SP_REGISTER << " r" << reg << " r" << reg << "\n";
}

// 構造体配列要素のメンバ(arr[i].member)の実アドレスをr{reg}に計算する
// アドレス = 配列先頭番地 + メンバオフセット(コンパイル時定数，member_accessが合成時にexpr->ivalへ保存済み)
//          + インデックス(実行時，member_access->children[0]->children[0]) × 構造体1要素分のバイト数
// レジスタ使用: r{reg}=インデックス→アドレス, r{reg+1}=定数(要素間隔・ベース，作業用)
void Generator::gen_struct_array_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs) {
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(member_access->loc);
    }
    const symbol_t *arr_sym = member_access->sym;          // 配列全体のシンボル(先頭番地・構造体名)
    node_t *array_access = member_access->children[0];     // arr[i] (children[0]=インデックス式)
    const int stride_bytes = this->struct_defs_.at(arr_sym->type.struct_name).total_words * 4;
    const int base_const = arr_sym->address + static_cast<int>(member_access->ival) * 4;  // 配列先頭+メンバオフセット

    // r{reg} = インデックス式 (保護するレジスタが指定されていれば，それらの値を評価中も保護する)
    this->gen_expr_protecting(array_access->children[0], reg, protect_regs);
    // r{reg+1} = 要素間隔(構造体1要素分のバイト数．2の冪とは限らないためmulで乗算する)
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << stride_bytes << "\n";
    (*this->out_) << "    mul r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // r{reg+1} = 配列先頭+メンバオフセット(コンパイル時定数)
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << base_const << "\n";
    // r{reg} = 実アドレス
    (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // フレーム上の配列の場合 (コンパイル時定数はフレーム内オフセットであり，実行時のSPが加わって番地になる)
    if (arr_sym->location != LOC_GLOBAL) {
        (*this->out_) << "    add " << SP_REGISTER << " r" << reg << " r" << reg << "\n";
    }
}

// 構造体メンバ配列アクセス(children.size()==2のND_ARRAY_ACCESS)の配列先頭アドレスをr{addr_reg}に載せる
// 通常の単一構造体変数のメンバ配列(entry.name[i])はコンパイル時アドレス確定，
// 構造体配列要素のメンバ配列(arr[i].name[j])は実行時アドレス計算になるため分岐する
void Generator::gen_member_array_base(node_t *expr, int addr_reg, const std::vector<int> &protect_regs) {
    node_t *designator = expr->children[1];   // ND_MEMBER_ACCESS
    if (designator->children[0]->kind == ND_ARRAY_ACCESS) {
        this->gen_struct_array_member_addr(designator, addr_reg, protect_regs);
    } else {
        this->gen_array_base_addr(addr_reg, designator->sym);
    }
}

// 配列要素の実アドレスをr{reg}に計算する
// アドレス = 配列先頭番地 + index * 要素サイズ(バイト単位)．配列先頭番地は配列の種類に応じて，
// コンパイル時定数・メモリからの読み出し・実行時計算のいずれかで求める
// レジスタ使用: r{reg}=index→アドレス, r{reg+1}=シフト量・配列先頭番地 (2本．構造体配列要素のメンバ配列の場合，
// 先頭番地の計算(gen_struct_array_member_addr)がさらにr{reg+2}を使うため3本必要)
// 読み書きの際の型に応じたマスクはここでは扱わない．求めたアドレスを使うレジスタ間接の読み込み・
// 書き込みの側が型から選ぶ(読み出しと書き込みでアドレス計算を共有するため)
void Generator::gen_array_elem_addr(node_t *expr, int reg, const std::vector<int> &protect_regs) {
    // 作業用のr{reg+2}が上限(r15)を超えないことを確認する
    if (reg + 2 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }

    // 要素型(意味解析が配列のbase型を注釈済み)ごとの要素サイズ(2^shift バイト)を決定する
    int shift;   // 要素サイズ(バイト)の2を底とする対数
    switch (expr->type.base) {
        case BASE_CHAR:  shift = 0; break;
        case BASE_SHORT: shift = 1; break;
        case BASE_INT:   shift = 2; break;
        default:
            throw std::string("compiler error: unsupported array element type at ")
                  + loc_to_string(expr->loc);
    }

    // r{reg} = index (r{reg+1}はまだ未使用)
    this->gen_expr_protecting(expr->children[0], reg, protect_regs);

    // オフセット = index * サイズ (サイズ1のcharはシフト不要)
    // 実行後: r{reg} = index * サイズ(バイトオフセット), r{reg+1} = シフト量(破棄可)
    if (shift > 0) {
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << shift << "\n";
        (*this->out_) << "    sll r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    }
    // 実行後: r{reg+1} = 配列先頭番地 (実行時計算の場合は，求めたオフセットのr{reg}も保護する)
    // 構造体のメンバ配列の場合
    if (expr->children.size() == 2) {
        std::vector<int> base_protect_regs = protect_regs;   // 先頭番地の計算中に保護するレジスタ
        // 求めたオフセットを持つr{reg}も保護対象に加える
        base_protect_regs.push_back(reg);
        // メンバ配列の先頭番地をr{reg+1}に求める
        this->gen_member_array_base(expr, reg + 1, base_protect_regs);
    }
    // 通常の配列・配列パラメータの場合
    else {
        // 配列の先頭番地をr{reg+1}に求める
        this->gen_array_base_addr(reg + 1, expr->sym);
    }
    // 実行後: r{reg} = 配列先頭番地 + オフセット = 実アドレス
    (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// グローバル変数とスタックがメモリ容量に収まるか検査する
// スタックはメモリの上端から下へ，グローバル変数は0番地から上へ伸びるため，両者が重なると
// 互いの値を壊す(ハードウェアは検出しない)．再帰があると深さが実行時にしか決まらず，
// 使用量を見積もれないため検査しない
void Generator::check_memory_usage() {
    std::set<std::string> path;   // 現在の探索経路(再帰の検出用)
    bool is_recursive = false;    // 探索中に再帰を見つけたか
    const int stack_bytes = this->stack_bytes_dfs("main", path, is_recursive);   // スタック使用量(最大)
    if (is_recursive) return;

    if (this->global_size_ + stack_bytes > RAM_SIZE) {
        throw std::string("compiler error: global variables (")
              + std::to_string(this->global_size_) + " bytes) and stack ("
              + std::to_string(stack_bytes) + " bytes) exceed memory capacity ("
              + std::to_string(RAM_SIZE) + " bytes)";
    }
}

// funcを呼び出してから戻るまでに使うスタックのバイト数(最大)を返す
// func自身のフレームに，呼び出し先の中で最も多く使うものの使用量(戻り先アドレスの4バイトを含む)を足す
int Generator::stack_bytes_dfs(const std::string &func, std::set<std::string> &path, bool &is_recursive) {
    // 現在の経路に既にfuncがあれば，直接・間接を問わず再帰であり，深さが定まらない
    if (path.count(func)) {
        is_recursive = true;
        return 0;
    }
    path.insert(func);   // 経路にfuncを追加してから呼び出し先を探索する

    int max_callee_bytes = 0;   // 呼び出し先のうち最も多いスタック使用量
    for (const std::string &callee : this->call_graph_.at(func)) {
        // 呼び出しごとに戻り先アドレス(4バイト)が積まれる
        const int callee_bytes = 4 + this->stack_bytes_dfs(callee, path, is_recursive);
        if (callee_bytes > max_callee_bytes) max_callee_bytes = callee_bytes;
    }

    path.erase(func);   // 探索し終えたので経路から外す(他の呼び出し経路と共有しないため)
    return this->func_frame_sizes_.at(func) + max_callee_bytes;
}

// 番地が実行時に決まる代入先(配列要素または構造体配列要素のメンバ)の実アドレスをr{reg}に計算する
void Generator::gen_runtime_addr(node_t *target, int reg, const std::vector<int> &protect_regs) {
    // 配列要素の場合
    if (target->kind == ND_ARRAY_ACCESS) {
        this->gen_array_elem_addr(target, reg, protect_regs);
    }
    // 構造体配列要素のメンバの場合
    else {
        this->gen_struct_array_member_addr(target, reg, protect_regs);
    }
}
