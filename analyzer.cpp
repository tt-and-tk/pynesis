#include <set>
#include <vector>

#include "analyzer.hpp"

// グローバル変数の配置開始アドレス (1変数=1ワード(4バイト)で順次割り当てる)
static const int g_global_base_addr = 0x0000000;

// ハードウェア変数の定義表 (ボードI/Oレジスタのみ公開，CPU内部レジスタは非公開)
// 読み書き可否はハードウェア実装(mypc/alu.svh)に従う．型はピンの状態を表すビット列として扱うため，全てunsigned int
static const std::vector<symbol_t> g_hw_vars = {
    // 名前        型                置き場所       番地   読み   書き
    {"BTN",       {BASE_INT, false}, LOC_REGISTER, 0x20, true,  false},  // タクトスイッチ
    {"DIPSW",     {BASE_INT, false}, LOC_REGISTER, 0x21, true,  false},  // DIPスイッチ
    {"LED",       {BASE_INT, false}, LOC_REGISTER, 0x22, false, true},   // LED
    {"RGBLED",    {BASE_INT, false}, LOC_REGISTER, 0x23, false, true},   // RGB LED
    {"PMOD_A",    {BASE_INT, false}, LOC_REGISTER, 0x24, true,  true},   // Pmod A
    {"PMOD_B",    {BASE_INT, false}, LOC_REGISTER, 0x25, true,  true},   // Pmod B
    {"AR8_13",    {BASE_INT, false}, LOC_REGISTER, 0x26, true,  true},   // Arduinoピン AR8～AR13
    {"AR_I2C",    {BASE_INT, false}, LOC_REGISTER, 0x27, true,  true},   // A，AR_SDA，AR_SCL
    {"AR0_7",     {BASE_INT, false}, LOC_REGISTER, 0x28, true,  true},   // Arduinoピン AR0～AR7
    {"AR_RST",    {BASE_INT, false}, LOC_REGISTER, 0x29, true,  false},  // Arduinoリセット
    {"AR_SPI",    {BASE_INT, false}, LOC_REGISTER, 0x2a, true,  true},   // AR_MISO，AR_SCK，AR_MOSI，AR_SS
    {"GPIO0_7",   {BASE_INT, false}, LOC_REGISTER, 0x2d, true,  true},   // GPIO0～7
    {"GPIO8_15",  {BASE_INT, false}, LOC_REGISTER, 0x2e, true,  true},   // GPIO8～15
    {"GPIO16_23", {BASE_INT, false}, LOC_REGISTER, 0x2f, true,  true},   // GPIO16～23
    {"GPIO24_27", {BASE_INT, false}, LOC_REGISTER, 0x30, true,  true},   // GPIO24～27
};

// 値を32ビットで折り返し，符号付きなら-2147483648〜2147483647，符号なしなら0〜4294967295の範囲に正規化する
static long long wrap32(long long value, bool is_signed) {
    const long long bits = value & 0xFFFFFFFFLL;   // 下位32ビット
    // 符号付きで最上位ビットが立っている場合は負の値として解釈する
    if (is_signed && bits > 0x7FFFFFFFLL) return bits - 0x100000000LL;
    return bits;
}

// 整数リテラルの値がint型になるかを返す (intで表せない0x80000000以上の値は16進でのみ書け，unsigned intになる)
static bool is_signed_literal(long long value) {
    return value <= 0x7FFFFFFFLL;
}

// 整数昇格後の型が符号付き(int)かどうかを返す
// 配列・構造体は整数の値ではないため，要素型によらず符号なしの演算の対象にしない
bool is_promoted_signed(const type_t &type) {
    return type.is_signed || type.base != BASE_INT || type.is_array;
}

// 二項演算を符号付きで行うかどうかを，各オペランドの昇格後の型が符号付きかどうかから返す
static bool is_signed_operation_by_signs(const std::string &op, bool is_lhs_signed, bool is_rhs_signed) {
    // シフトは左オペランドの型に従う (シフト量の型は結果に影響しない)
    if (op == "<<" || op == ">>") return is_lhs_signed;
    return is_lhs_signed && is_rhs_signed;
}

// 二項演算を符号付きで行うかどうかを返す
bool is_signed_operation(const std::string &op, const type_t &lhs, const type_t &rhs) {
    return is_signed_operation_by_signs(op, is_promoted_signed(lhs), is_promoted_signed(rhs));
}

// コンストラクタ: ASTを受け取る
Analyzer::Analyzer(node_t *root) : root_(root), next_addr_(g_global_base_addr) {}

// 意味解析を実行してシンボルテーブルを返す
std::map<std::string, const symbol_t *> Analyzer::operator()() {
    // ハードウェア変数をあらかじめシンボルテーブルに登録する (静的領域の実体を直接指す)
    for (const symbol_t &hw : g_hw_vars) {
        this->symbols_[hw.name] = &hw;
    }

    // 1パス目: グローバル宣言の索引を作り，名前の重複を検査する
    this->index_global_decls();

    // 2パス目: const変数・構造体定義・グローバル変数の登録と関数名の収集を行う
    this->collect_globals();

    // プログラムの開始点となるmain関数が必要
    if (this->func_names_.find("main") == this->func_names_.end()) {
        throw std::string("compiler error: 'main' function is not defined");
    }

    // 3パス目: 各関数本体を検査する
    // (analyze_expr内のND_CALLケースが，通りがけに全関数の呼び出し先をcall_graph_へ記録する．
    //  ここまで完了した時点で，どの関数がどの関数を呼ぶかの記録がすべて出揃っている)
    this->analyze_functions();

    // 呼び出しグラフを検査する (再帰の検出，最大ネスト段数の超過検出)
    this->check_call_depth();

    // レジスタ退避領域を，全変数のアドレス割り当て後の空き番地に確保する
    this->scratch_base_ = this->next_addr_;
    this->next_addr_ += MAX_REG * 4;

    // 全変数の合計アドレスがアドレス空間(28ビット)を超えていないか確認する
    if (this->next_addr_ > 0x10000000) {
        throw std::string("compiler error: total variable memory exceeds address space (28-bit)");
    }

    return this->symbols_;
}

// パラメータシンボル表を返す
const std::map<std::string, std::vector<const symbol_t *>> &Analyzer::func_params() const {
    return this->func_params_;
}

// 構造体定義表を返す
const std::map<std::string, struct_def_t> &Analyzer::struct_defs() const {
    return this->struct_defs_;
}

// レジスタ退避領域の先頭番地を返す
int Analyzer::scratch_base() const {
    return this->scratch_base_;
}

// 1パス目: プログラム直下の宣言の索引を作り，名前の重複を検査する
// 後方で宣言された変数・構造体を宣言順によらず解決できるよう，宣言ノードを名前から引けるようにしておく．
// 変数(const変数を含む)・関数・ハードウェア変数は同じ名前空間，構造体名はそれとは別の名前空間として検査する
void Analyzer::index_global_decls() {
    for (node_t *child : this->root_->children) {
        // 構造体定義の場合 (構造体名は変数・関数とは別の名前空間のため，構造体名どうしでのみ重複を検査する)
        if (child->kind == ND_STRUCT_DECL) {
            // 同じ名前の構造体が定義済みの場合 (同じ名前の構造体を再定義することは禁止する)
            if (this->struct_decl_nodes_.count(child->sval)) {
                throw std::string("compiler error: redefinition of struct '") + child->sval
                      + "' at " + loc_to_string(child->loc);
            }
            this->struct_decl_nodes_[child->sval] = child;
            continue;
        }

        // 名前の重複チェック (変数・関数・ハードウェア変数の全てと衝突しないこと)
        if (this->symbols_.count(child->sval) || this->global_var_decls_.count(child->sval)
            || this->func_names_.count(child->sval)) {
            throw std::string("compiler error: redefinition of '") + child->sval
                  + "' at " + loc_to_string(child->loc);
        }
        // 変数宣言(const変数を含む)の場合
        if (child->kind == ND_VAR_DECL) {
            this->global_var_decls_[child->sval] = child;
        } else {
            // 関数定義: 関数名と戻り値型を登録する
            this->func_names_[child->sval] = child->type;
        }
    }
}

// 宣言ノードの型を確定させる (確定済みなら何もしない)
// 構造体型なら構造体定義を解決し，配列なら要素数を文字列リテラルの長さ，または定数式から計算して畳み込む
void Analyzer::resolve_decl_type(node_t *decl) {
    // 構造体型の場合 (メンバ構成が確定しないとサイズを求められないため，先に構造体定義を解決する)
    if (decl->type.base == BASE_STRUCT) {
        // 宣言されている構造体が定義済みか確認する
        if (!this->struct_decl_nodes_.count(decl->type.struct_name)) {
            throw std::string("compiler error: use of undeclared struct '") + decl->type.struct_name
                  + "' at " + loc_to_string(decl->loc);
        }
        this->resolve_struct_def(decl->type.struct_name);
    }
    // スカラー，または要素数が確定済みの配列の場合
    // (スカラーは確定させる要素数を持たず，確定済みの配列は計算し直す必要がないため，これ以上何もしない．
    //  配列の要素数は正の値に限るため，0は未確定を表す)
    if (!decl->type.is_array || decl->type.array_size > 0) return;

    // 文字列リテラルで初期化されている場合 (要素数は文字列長から決まるため，定数式を計算しない)
    if (!decl->children.empty() && decl->children[0]->kind == ND_STRING_LIT) {
        // 文字列リテラルによる初期化: char msg[] = "hello";
        // char以外の配列の場合
        if (decl->type.base != BASE_CHAR) {
            throw std::string("compiler error: string literal can only initialize char array at ")
                  + loc_to_string(decl->children[0]->loc);
        }
        // サイズは文字列長 + 1(ヌル終端)
        decl->type.array_size = static_cast<int>(decl->children[0]->sval.size()) + 1;
        return;
    }

    // サイズ明示の配列宣言: int table[10];
    this->begin_resolving(decl);
    const long long size = this->eval_const_expr(decl->children[0]).value;   // 配列の要素数
    // 要素数が正でない場合
    if (size <= 0) {
        throw std::string("compiler error: array size must be positive at ")
              + loc_to_string(decl->children[0]->loc);
    }
    // 要素数がintで表せない場合 (0x80000000以上の符号なしの値)
    if (size > 0x7FFFFFFFLL) {
        throw std::string("compiler error: array size is too large at ")
              + loc_to_string(decl->children[0]->loc);
    }
    decl->type.array_size = static_cast<int>(size);
    // サイズ式を畳み込み済みリテラルに置き換える
    node_t *folded = new node_t;
    folded->kind = ND_INT_LIT;
    folded->ival = size;
    folded->loc = decl->children[0]->loc;
    decl->children[0] = folded;
    this->end_resolving(decl);
}

// 構造体定義のメンバ構成を確定させ，struct_defs_に登録する (登録済みなら何もしない)
// メンバのオフセット(構造体先頭からのワード数)と構造体全体のワード数をここで確定させる
// 無名構造体に続けて即座に変数宣言されていた場合，その変数自体は別のND_VAR_DECLノードとして
// root_の子に並んでいる(パーサが生成)ため，ここでは構造体定義(ND_STRUCT_DECL)だけを扱えばよく，
// 変数宣言側は2パス目のcollect_globalsが通常の構造体変数宣言と同じ経路で処理する
void Analyzer::resolve_struct_def(const std::string &name) {
    // 構造体定義が登録済みの場合 (メンバ構成は確定済みで，同じ定義を計算し直す必要がないため，これ以上何もしない)
    if (this->struct_defs_.count(name)) return;

    const node_t *decl = this->struct_decl_nodes_.at(name);   // 構造体定義ノード
    // 構造体定義の解決を始める (メンバの配列サイズがこの構造体自身に依存する循環参照を検出する)
    this->begin_resolving(decl);

    // メンバがない状態から構造体定義を組み立てる
    struct_def_t def;
    def.total_words = 0;

    // メンバを宣言順に登録し，構造体先頭からのオフセットを決める
    for (node_t *member : decl->children) {
        for (const struct_member_t &registered : def.members) {
            // 登録済みのメンバと同じ名前の場合
            if (registered.name == member->sval) {
                throw std::string("compiler error: duplicate member '") + member->sval
                      + "' in struct '" + name + "' at " + loc_to_string(member->loc);
            }
        }

        // 配列メンバのサイズを定数式として確定する (変数宣言の配列サイズと同じ扱い)
        this->resolve_decl_type(member);

        const int words = member->type.is_array ? Analyzer::calc_array_words(member->type) : 1;   // メンバが占めるワード数
        // それまでのメンバの合計ワード数をオフセットとしてメンバを登録し，合計ワード数を進める
        def.members.push_back({member->sval, member->type, def.total_words});
        def.total_words += words;
    }

    // 組み立てた構造体定義を登録し，解決を終える
    this->struct_defs_[name] = def;
    this->end_resolving(decl);
}

// グローバルのconst変数を名前から解決してシンボルを返す (値が未確定なら初期化子を計算して登録する)
// 該当するグローバルのconst変数の宣言がなければnullptrを返す
const symbol_t *Analyzer::resolve_global_const(const std::string &name) {
    const auto it = this->global_var_decls_.find(name);
    // 該当する宣言がない，または通常の変数の宣言の場合 (解決すべきconst変数がないため，これ以上何もしない)
    if (it == this->global_var_decls_.end() || !it->second->type.is_const) return nullptr;

    node_t *decl = it->second;   // const変数の宣言ノード
    // 値が未確定の場合 (確定済みなら登録済みのシンボルをそのまま返す)
    if (decl->sym == nullptr) {
        this->begin_resolving(decl);
        symbol_t *sym = this->register_const_var(decl);
        this->symbols_[name] = sym;
        decl->sym = sym;
        this->end_resolving(decl);
    }
    return decl->sym;
}

// 宣言の解決を始める
// 解決中の宣言に再び到達した場合は，宣言の型・値が自身に依存しているため循環参照としてエラーにする
void Analyzer::begin_resolving(const node_t *decl) {
    // 解決中の宣言に再び到達した場合
    if (this->resolving_decls_.count(decl)) {
        // 無名構造体の名前はパーサが割り当てた内部名でソースに現れないため，無名であることを示す
        const std::string name = (decl->kind == ND_STRUCT_DECL && decl->sval[0] == '$')
                                     ? "anonymous struct" : "'" + decl->sval + "'";   // エラーメッセージに表示する宣言名
        throw std::string("compiler error: circular reference in declaration of ") + name
              + " at " + loc_to_string(decl->loc);
    }
    this->resolving_decls_.insert(decl);
}

// 宣言の解決を終える
void Analyzer::end_resolving(const node_t *decl) {
    this->resolving_decls_.erase(decl);
}

// 構造体型の変数1つ分(配列宣言ならその配列全体分)のアドレスを確保し，シンボルを生成して返す
// (シンボル表への格納自体はグローバル用のcollect_globals・ローカル用のanalyze_local_declが
//  それぞれ自分のシンボル表(symbols_・scopes_)へ行うため，この関数ではまだ行わない)
symbol_t *Analyzer::register_struct_var(node_t *decl, location_t location) {
    // 宣言されている構造体の定義と，構造体配列なら要素数を確定させる (通常の配列宣言のサイズ指定と同じ扱い)
    this->resolve_decl_type(decl);
    const int element_count = decl->type.is_array ? decl->type.array_size : 1;   // 配列の要素数 (配列でなければ1)

    // 構造体変数(配列なら配列全体)のアドレスを確保し，メンバ構成込みの型情報を持つシンボルを生成する
    symbol_t *sym = new symbol_t{decl->sval, decl->type, location, this->next_addr_, true, true};
    // 構造体1個分のワード数×要素数ぶん，次に割り当てるアドレスを進める
    // (メンバのメモリレイアウトは../specification/compiler.mdの「構造体」節を参照)
    this->next_addr_ += this->struct_defs_.at(decl->type.struct_name).total_words * 4 * element_count;
    // 生成したシンボルは，呼び出し元がグローバル/ローカルいずれかのシンボル表へ格納する
    return sym;
}

// const変数の初期化子を定数式として計算し，値を持つシンボルを生成して返す
// メモリ番地は割り当てず，値は参照箇所(analyze_exprのND_VAR)で整数リテラルとして埋め込まれる
symbol_t *Analyzer::register_const_var(const node_t *decl) {
    const node_t *init = decl->children[0];                          // 初期化子の式
    const long long value = this->eval_const_expr(init).value;       // 初期化子を計算した値

    // 値を宣言した型の範囲に収める (範囲外の値を黙って切り詰めると，同じ型の通常の変数と値が食い違うため)
    long long min_value;   // 宣言した型で表せる最小値
    long long max_value;   // 宣言した型で表せる最大値
    switch (decl->type.base) {
        case BASE_CHAR:  min_value = -128LL;        max_value = 127LL;        break;
        case BASE_SHORT: min_value = -32768LL;      max_value = 32767LL;      break;
        case BASE_INT:   min_value = -2147483648LL; max_value = 2147483647LL; break;
        default:
            throw std::string("compiler error: unsupported const variable type at ")
                  + loc_to_string(decl->loc);
    }
    // 符号なし型の場合は，0〜(2^ビット幅-1)の範囲にする (符号付きの最大値の2倍+1が2^ビット幅-1になる)
    if (!decl->type.is_signed) {
        max_value = max_value * 2 + 1;
        min_value = 0;
    }
    // 値が型の範囲外の場合
    if (value < min_value || value > max_value) {
        throw std::string("compiler error: value of const variable '") + decl->sval
              + "' is out of range for its type at " + loc_to_string(init->loc);
    }

    // 値は32ビットのビット列としてintに保持し，読み出す際に型に応じて解釈する(const_symbol_value)
    // 読み取り専用のシンボルにすることで，代入・++/--・scan等の書き込みを既存の検査でエラーにする
    const int bits = static_cast<int>(static_cast<unsigned int>(value));   // 値の32ビットのビット列
    return new symbol_t{decl->sval, decl->type, LOC_CONST, bits, true, false};
}

// const変数のシンボルが保持する32ビットのビット列を，型に応じた値として返す
long long Analyzer::const_symbol_value(const symbol_t *sym) {
    // unsigned intは最上位ビットが立っていても正の値として解釈する (unsigned char/shortは常にintの正の範囲に収まる)
    if (!is_promoted_signed(sym->type)) {
        return static_cast<unsigned int>(sym->address);
    }
    return sym->address;
}

// 2パス目: プログラム直下を宣言順に走査し，const変数・構造体定義・グローバル変数の登録と関数名の収集を行う
// 先に全グローバルを登録することで，関数本体からの前方参照(後ろで宣言された変数の使用)を可能にする．
// 後方の宣言が先に参照された場合，その宣言はこの走査で到達するより前に参照した時点で解決済みになっている
// (名前の重複は1パス目のindex_global_declsで検査済み)
void Analyzer::collect_globals() {
    for (node_t *child : this->root_->children) {
        // 構造体定義: メンバ構成を確定させる
        if (child->kind == ND_STRUCT_DECL) {
            this->resolve_struct_def(child->sval);
        }
        // グローバル変数宣言: 初期化子を評価し，アドレスを割り当てて登録する
        else if (child->kind == ND_VAR_DECL) {
            if (child->type.is_const) {
                // const変数: 値を確定させて登録する (メモリ番地は割り当てない)
                this->resolve_global_const(child->sval);
            } else if (child->type.base == BASE_STRUCT) {
                // 構造体変数(配列宣言含む): 初期化子は非対応のため，メンバ構成に基づくアドレス確保のみ行う
                symbol_t *sym = this->register_struct_var(child, LOC_GLOBAL);
                this->symbols_[child->sval] = sym;
                child->sym = sym;
            } else if (child->type.is_array) {
                // 配列の要素数を確定させる (先に参照されて解決済みなら何もしない)
                this->resolve_decl_type(child);
                // アドレスを割り当てて登録する (確保ワード数は型に応じて計算)
                symbol_t *sym =
                    new symbol_t{child->sval, child->type, LOC_GLOBAL, this->next_addr_, true, true};
                this->symbols_[child->sval] = sym;
                child->sym = sym;
                this->next_addr_ += Analyzer::calc_array_words(child->type) * 4;
            } else {
                // スカラー変数: 初期化子があればコンパイル時に計算し，リテラルに置き換える(定数畳み込み)
                if (!child->children.empty()) {
                    const const_value_t init = this->eval_const_expr(child->children[0]);   // 初期化子を計算した値
                    node_t *folded = new node_t;
                    folded->kind = ND_INT_LIT;
                    folded->ival = init.value;
                    folded->type = type_t{BASE_INT, init.is_signed};
                    folded->loc = child->children[0]->loc;
                    // 差し替え前の旧部分木はあえて解放しない
                    // (ASTは全ノードをdeleteせず，プロセス終了時のOS回収に任せる方針のため)
                    child->children[0] = folded;  // 初期化式の子要素を計算済みのリテラルで更新する
                }
                // ソース宣言のグローバル変数はnewでヒープ確保し解放しない
                symbol_t *sym =
                    new symbol_t{child->sval, child->type, LOC_GLOBAL, this->next_addr_, true, true};
                this->symbols_[child->sval] = sym;
                child->sym = sym;   // 宣言ノード自身もシンボルを指す (コード生成でアドレス参照に使う)
                this->next_addr_ += 4;   // 型に関係なく1変数=1ワード(4バイト)使う
            }
        }
        // 関数定義: パラメータのシンボルを登録する (関数名・戻り値型は1パス目のindex_global_declsで登録済み)
        // 呼び出し側の引数検査(analyze_expr の ND_CALL)は3パス目より前に全関数のパラメータが必要なため，
        // パラメータの番地割り当てもここ(2パス目)で行う．3パス目(analyze_functions)はここで作った
        // シンボルをスコープに積んで本体を検査するだけになる
        else if (child->kind == ND_FUNC_DEF) {
            std::vector<const symbol_t *> params;
            for (size_t i = 0; i + 1 < child->children.size(); i++) {
                node_t *param = child->children[i];

                // 同一関数内でのパラメータ名重複はエラー
                for (const symbol_t *p : params) {
                    if (p->name == param->sval) {
                        throw std::string("compiler error: duplicate parameter name '") + param->sval
                              + "' at " + loc_to_string(param->loc);
                    }
                }
                // ハードウェア変数と同名のパラメータは禁止する (I/Oレジスタの誤上書き防止)
                const auto hw_it = this->symbols_.find(param->sval);
                if (hw_it != this->symbols_.end() && hw_it->second->location == LOC_REGISTER) {
                    throw std::string("compiler error: cannot shadow hardware register '") + param->sval
                          + "' at " + loc_to_string(param->loc);
                }

                symbol_t *sym = new symbol_t{param->sval, param->type, LOC_LOCAL, this->next_addr_, true, true};
                this->next_addr_ += 4;
                param->sym = sym;
                params.push_back(sym);
            }
            this->func_params_[child->sval] = params;
        }
    }
}

// コンパイル時に値が確定する定数式を計算して値を返す (定数畳み込み)
// 呼び出し元は定数式が要求される文脈 (配列サイズ・初期化子・case値等．一覧は../specification/compiler.mdの「定数式」節を参照)
// 実行時の演算と結果が一致するよう，整数昇格と符号の規則に従い，32ビットで折り返して計算する．
// 通常の変数の参照や関数呼び出しなど，コンパイル時に値が確定しない式を含む場合はエラーにする．
// const変数の参照は，その値として計算する．
// また sizeof(変数名) も例外で許可する．sizeofが必要とするのは変数の「値」ではなく「型のサイズ」であり，
// 型は変数の値と無関係にシンボルテーブルから分かるため，変数参照であってもコンパイル時に確定できるため
// (いずれも参照先が後方で宣言されたグローバルの宣言なら，その時点で宣言ノードから型・値を解決する)
const_value_t Analyzer::eval_const_expr(const node_t *expr) {
    // 整数リテラル
    if (expr->kind == ND_INT_LIT) {
        return {expr->ival, is_signed_literal(expr->ival)};
    }
    // 文字リテラルはintへ昇格した値
    if (expr->kind == ND_CHAR_LIT) {
        return {expr->ival, true};
    }

    // const変数の参照は値を返す
    if (expr->kind == ND_VAR) {
        const symbol_t *sym = this->lookup_symbol(expr->sval);
        // シンボル表にない場合 (未解決の後方のグローバルのconst変数でありうるため，宣言から解決を試みる)
        if (sym == nullptr) {
            sym = this->resolve_global_const(expr->sval);
        }
        // シンボル表にも宣言にもない名前の場合 (未宣言なのでエラーにする)
        if (sym == nullptr && !this->global_var_decls_.count(expr->sval)) {
            throw std::string("compiler error: use of undeclared identifier '") + expr->sval
                  + "' at " + loc_to_string(expr->loc);
        }
        // const変数の場合
        if (sym != nullptr && sym->location == LOC_CONST) {
            return {Analyzer::const_symbol_value(sym), is_promoted_signed(sym->type)};
        }
        // 通常の変数は値がコンパイル時に確定しないので，「定数式でない」エラーとして扱う
    }

    // sizeof: 型名，または変数名の型サイズをコンパイル時に返す (式自体は評価しない．結果はint)
    if (expr->kind == ND_SIZEOF) {
        if (expr->children.empty()) {
            // sizeof(型名)
            return {this->type_size_bytes(expr->type), true};
        }
        // sizeof(変数名): 値ではなく型だけが必要なのでND_VARのみ許可する
        const node_t *inner = expr->children[0];
        if (inner->kind != ND_VAR) {
            throw std::string("compiler error: sizeof argument in a constant expression "
                               "must be a type name or variable name at ")
                  + loc_to_string(inner->loc);
        }
        const symbol_t *sym = this->lookup_symbol(inner->sval);
        // シンボル表に登録済みの変数の場合
        if (sym != nullptr) {
            // 関数引数の配列が使用される可能性もあるのでそれをチェック
            if (sym->type.is_array && sym->type.array_size == 0) {
                throw std::string("compiler error: sizeof of an array parameter (size unknown) at ")
                      + loc_to_string(inner->loc);
            }
            return {this->type_size_bytes(sym->type), true};
        }
        // まだ登録されていないグローバルの宣言は，宣言ノードから型を確定させてサイズを求める
        const auto it = this->global_var_decls_.find(inner->sval);
        // グローバルの宣言もない場合
        if (it == this->global_var_decls_.end()) {
            throw std::string("compiler error: use of undeclared identifier '") + inner->sval
                  + "' at " + loc_to_string(inner->loc);
        }
        this->resolve_decl_type(it->second);
        return {this->type_size_bytes(it->second->type), true};
    }

    // 前置単項演算
    if (expr->kind == ND_UNOP) {
        const const_value_t v = this->eval_const_expr(expr->children[0]);
        // -・+・~ の結果はオペランドの昇格後の型，! の結果はint
        if      (expr->sval == "-") return {wrap32(-v.value, v.is_signed), v.is_signed};
        else if (expr->sval == "+") return v;
        else if (expr->sval == "~") return {wrap32(~v.value, v.is_signed), v.is_signed};
        else if (expr->sval == "!") return {(v.value == 0) ? 1 : 0, true};
        // ++/-- は変数にしか使えないので定数式では不可 (下のエラーに落ちる)
    }

    // 二項演算
    if (expr->kind == ND_BINOP) {
        const const_value_t l = this->eval_const_expr(expr->children[0]);
        const const_value_t r = this->eval_const_expr(expr->children[1]);
        return Analyzer::eval_const_binop(expr, l, r);
    }

    // 三項演算: 結果の型は両分岐の値の型から決まるため，選ばれない分岐も計算する
    if (expr->kind == ND_TERNARY) {
        const const_value_t cond = this->eval_const_expr(expr->children[0]);
        const const_value_t then_value = this->eval_const_expr(expr->children[1]);
        const const_value_t else_value = this->eval_const_expr(expr->children[2]);
        const bool is_signed = then_value.is_signed && else_value.is_signed;                // 結果が符号付きか
        const long long chosen = (cond.value != 0) ? then_value.value : else_value.value;   // 選ばれた分岐の値
        return {wrap32(chosen, is_signed), is_signed};
    }

    // 通常の変数参照・関数呼び出し等はコンパイル時に値が確定しないのでエラー
    throw std::string("compiler error: expression must be a constant expression at ")
          + loc_to_string(expr->loc);
}

// 二項演算の定数式を，実行時の演算と同じ規則で計算する
// 符号なしの演算では，両辺を符号なし32ビットの値として解釈し直してから計算する
const_value_t Analyzer::eval_const_binop(const node_t *expr, const const_value_t &l, const const_value_t &r) {
    const std::string &op = expr->sval;   // 演算子
    const bool is_signed = is_signed_operation_by_signs(op, l.is_signed, r.is_signed);   // 符号付きで演算するか
    const long long lv = wrap32(l.value, is_signed);   // 演算の符号で解釈した左辺
    const long long rv = wrap32(r.value, is_signed);   // 演算の符号で解釈した右辺

    // 論理演算・比較の結果は0/1のint
    if (op == "&&") return {(lv != 0 && rv != 0) ? 1 : 0, true};
    if (op == "||") return {(lv != 0 || rv != 0) ? 1 : 0, true};
    if (op == "==") return {(lv == rv) ? 1 : 0, true};
    if (op == "!=") return {(lv != rv) ? 1 : 0, true};
    if (op == "<")  return {(lv < rv) ? 1 : 0, true};
    if (op == ">")  return {(lv > rv) ? 1 : 0, true};
    if (op == "<=") return {(lv <= rv) ? 1 : 0, true};
    if (op == ">=") return {(lv >= rv) ? 1 : 0, true};

    // 除算・剰余: 実行時にCPUが停止する，または結果が定まらない組み合わせはコンパイル時に検出する
    if (op == "/" || op == "%") {
        // ゼロ除算の場合
        if (rv == 0) {
            throw std::string("compiler error: division by zero at ") + loc_to_string(expr->loc);
        }
        // 符号付きの最小値を-1で割る場合 (商が32ビットで表せない)
        if (is_signed && lv == -2147483648LL && rv == -1) {
            throw std::string("compiler error: division overflow at ") + loc_to_string(expr->loc);
        }
    }

    // 乗算・左シフトは64ビットの符号付き整数でも桁あふれしうるため，符号なし64ビットで計算して下位32ビットを得る
    const unsigned long long ulv = static_cast<unsigned long long>(lv);   // 符号なし64ビットで扱う左辺
    const unsigned long long urv = static_cast<unsigned long long>(rv);   // 符号なし64ビットで扱う右辺
    long long result;   // 32ビットで折り返す前の結果
    if      (op == "+")  result = lv + rv;
    else if (op == "-")  result = lv - rv;
    else if (op == "*")  result = static_cast<long long>((ulv * urv) & 0xFFFFFFFFULL);
    else if (op == "/")  result = lv / rv;
    else if (op == "%")  result = lv % rv;
    else if (op == "&")  result = lv & rv;
    else if (op == "|")  result = lv | rv;
    else if (op == "^")  result = lv ^ rv;
    // シフト量は実行時と同じく下位5ビット(0〜31)のみを使う．右シフトは符号付きなら算術，符号なしなら論理になる
    else if (op == "<<") result = static_cast<long long>((ulv << (rv & 31)) & 0xFFFFFFFFULL);
    else if (op == ">>") result = lv >> (rv & 31);
    else {
        throw std::string("compiler error: expression must be a constant expression at ")
              + loc_to_string(expr->loc);
    }
    return {wrap32(result, is_signed), is_signed};
}

// 配列が占有するワード数を計算する (int=1要素1ワード, short=2要素1ワード, char=4要素1ワード)
int Analyzer::calc_array_words(const type_t &type) {
    const int n = type.array_size;
    switch (type.base) {
        case BASE_INT:   return n;             // 32ビット: 1要素=1ワード
        case BASE_SHORT: return (n + 1) / 2;   // 16ビット: 2要素=1ワード
        case BASE_CHAR:  return (n + 3) / 4;   // 8ビット: 4要素=1ワード
        default:
            throw std::string("compiler error: unsupported array element type");
    }
}

// 型の論理バイト数を返す (sizeof用．C言語準拠で実メモリのワード境界は考慮しない)
// 配列は「要素数 × 要素型のバイト数」を返す．構造体はメンバの合計ワード数から求める(struct_defs_の参照が必要)
int Analyzer::type_size_bytes(const type_t &type) const {
    if (type.base == BASE_STRUCT) {
        // BASE_STRUCT型の型情報はresolve_decl_typeが構造体の存在を検証して定義を解決した後にしか
        // 使われないため(未定義の構造体はそこで既にコンパイルエラーになる)，ここに渡ってくる
        // typeのstruct_nameは常に登録済みであり，探索に失敗することはない．
        // 構造体配列は「構造体1個分のバイト数×要素数」を返す
        const int struct_bytes = this->struct_defs_.at(type.struct_name).total_words * 4;
        return type.is_array ? struct_bytes * type.array_size : struct_bytes;
    }
    int elem_bytes;
    switch (type.base) {
        case BASE_CHAR:  elem_bytes = 1; break;
        case BASE_SHORT: elem_bytes = 2; break;
        case BASE_INT:   elem_bytes = 4; break;
        default:
            throw std::string("compiler error: sizeof of unsupported type");
    }
    return type.is_array ? elem_bytes * type.array_size : elem_bytes;
}

// 3パス目: 各関数本体を検査する
// ND_FUNC_DEFのchildren = [param0, param1, ..., block] (パラメータがなければchildren[0]がブロック)
void Analyzer::analyze_functions() {
    for (node_t *child : this->root_->children) {
        if (child->kind != ND_FUNC_DEF) continue;

        // 現在解析中の関数名・戻り値型を記録する (呼び出しグラフ構築・return文の整合性検査用)
        this->current_function_ = child->sval;
        this->current_return_type_ = child->type;
        this->call_graph_[child->sval];   // 呼び出し先が無い関数もグラフに登録しておく(空集合)

        // 関数スコープを開く (パラメータと本体のローカル変数が同じスコープに入る)
        this->scopes_.push_back({});

        // パラメータをスコープに登録する (シンボル自体は2パス目のcollect_globalsで作成済み)
        for (const symbol_t *sym : this->func_params_[child->sval]) {
            this->scopes_.back()[sym->name] = sym;
        }

        // 関数本体ブロック(最後の子)を検査する
        this->analyze_block(child->children.back());

        // 関数スコープを閉じる
        this->scopes_.pop_back();
    }
}

// 呼び出しグラフを検査する (再帰の検出，最大ネスト段数MAX_CALL_DEPTHの超過検出)
// mainを起点に深さ優先探索する(mainはCALLされないため，ネスト段数の起点として数えない)
void Analyzer::check_call_depth() {
    std::set<std::string> path;   // 現在の呼び出し経路(再帰検出用)
    this->check_call_depth_dfs("main", path);
}

// funcから辿れる呼び出し経路を深さ優先探索し，再帰とネスト段数超過を検査する
// pathには現在の探索経路上にある関数名が入っている(再帰=pathに既にある関数への到達で検出する)
// 戻り値: funcを起点とした場合の最大ネスト段数(func自身は含まず，呼び出し先の段数のみ)
int Analyzer::check_call_depth_dfs(const std::string &func, std::set<std::string> &path) {
    // 現在の経路に既にfuncがあれば，直接・間接を問わず再帰(循環)
    if (path.count(func)) {
        throw std::string("compiler error: recursive function call detected involving '") + func + "'";
    }

    path.insert(func);   // 経路にfuncを追加してから子を探索する

    int max_depth = 0;   // funcの呼び出し先の中で最も深いネスト段数
    for (const std::string &callee : this->call_graph_[func]) {
        const int callee_depth = this->check_call_depth_dfs(callee, path);
        if (callee_depth + 1 > max_depth) {
            max_depth = callee_depth + 1;
        }
    }

    path.erase(func);   // 探索し終えたので経路から外す(他の兄弟経路と共有しないため)

    if (max_depth > MAX_CALL_DEPTH) {
        throw std::string("compiler error: function call nesting exceeds maximum depth (")
              + std::to_string(MAX_CALL_DEPTH) + ") at '" + func + "'";
    }

    return max_depth;
}

// ブロックを検査する (新しいローカルスコープを積み，抜けるときに捨てる)
void Analyzer::analyze_block(node_t *block) {
    this->scopes_.push_back({});   // 新しいスコープを積む
    for (node_t *stmt : block->children) {
        this->analyze_stmt(stmt);
    }
    this->scopes_.pop_back();      // スコープを捨てる
}

// 文を検査する
void Analyzer::analyze_stmt(node_t *stmt) {
    // 変数宣言
    if (stmt->kind == ND_VAR_DECL) {
        this->analyze_local_decl(stmt);
    }
    // ブロック (入れ子の { ... })
    else if (stmt->kind == ND_BLOCK) {
        this->analyze_block(stmt);
    }
    // return文: 関数の戻り値型とreturn文の有無・値が一致するか検査する
    else if (stmt->kind == ND_RETURN) {
        if (stmt->children.empty()) {
            // void関数はreturn文自体が任意なので，値なしのreturn;はvoid以外のときのみエラー
            if (this->current_return_type_.base != BASE_VOID) {
                throw std::string("compiler error: non-void function '") + this->current_function_
                      + "' must return a value at " + loc_to_string(stmt->loc);
            }
        } else {
            if (this->current_return_type_.base == BASE_VOID) {
                throw std::string("compiler error: void function '") + this->current_function_
                      + "' cannot return a value at " + loc_to_string(stmt->loc);
            }
            this->analyze_expr(stmt->children[0]);
        }
    }
    // if文 (children: 条件, then節, [else節])
    else if (stmt->kind == ND_IF) {
        this->analyze_expr(stmt->children[0]);     // 条件
        this->analyze_stmt(stmt->children[1]);     // then節
        if (stmt->children.size() == 3) {
            this->analyze_stmt(stmt->children[2]); // else節
        }
    }
    // while文 (children: 条件, 本体)
    else if (stmt->kind == ND_WHILE) {
        this->analyze_expr(stmt->children[0]);     // 条件
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[1]);     // 本体
        this->loop_depth_--;
    }
    // for文 (children: 初期化, 条件, 更新, 本体．省略された部分はnullptr)
    else if (stmt->kind == ND_FOR) {
        // for全体で1つのスコープを張る (初期化部で宣言した変数を条件・更新・本体から見えるようにする)
        this->scopes_.push_back({});
        if (stmt->children[0]) this->analyze_stmt(stmt->children[0]);  // 初期化
        if (stmt->children[1]) this->analyze_expr(stmt->children[1]);  // 条件
        if (stmt->children[2]) this->analyze_expr(stmt->children[2]);  // 更新
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[3]);                         // 本体
        this->loop_depth_--;
        this->scopes_.pop_back();
    }
    // do-while文 (children: 本体, 条件)
    else if (stmt->kind == ND_DO_WHILE) {
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[0]);     // 本体
        this->loop_depth_--;
        this->analyze_expr(stmt->children[1]);     // 条件
    }
    // switch文
    else if (stmt->kind == ND_SWITCH) {
        this->analyze_switch(stmt);
    }
    // break文 (ループまたはswitchの中でのみ許される)
    else if (stmt->kind == ND_BREAK) {
        if (this->loop_depth_ == 0 && this->switch_depth_ == 0) {
            throw std::string("compiler error: 'break' outside loop or switch at ")
                  + loc_to_string(stmt->loc);
        }
    }
    // continue文 (ループの中でのみ許される)
    else if (stmt->kind == ND_CONTINUE) {
        if (this->loop_depth_ == 0) {
            throw std::string("compiler error: 'continue' outside loop at ")
                  + loc_to_string(stmt->loc);
        }
    }
    // それ以外は式文として検査する
    else {
        this->analyze_expr(stmt);
    }
}

// switch文を検査する (children: 条件式, 本体の文とcase/defaultラベルが平坦に並ぶ)
void Analyzer::analyze_switch(node_t *stmt) {
    this->analyze_expr(stmt->children[0]);   // 条件式

    this->switch_depth_++;            // switchの中ではbreakが許される
    this->scopes_.push_back({});      // switch本体のスコープ

    std::set<long long> case_values;  // case値の32ビットのビット列 (重複検出用)
    bool has_default = false;         // defaultの重複検出用

    // 本体(children[1..])を順に検査する
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *child = stmt->children[i];
        // case節: 値は定数式．畳み込んで重複チェックし，結果をivalに保存する
        if (child->kind == ND_CASE) {
            const long long v = this->eval_const_expr(child->children[0]).value;
            // 条件の値とはビット列の一致で比較するため，ビット列が同じcase値を重複とみなす(-1と0xFFFFFFFF等)
            const long long bits = v & 0xFFFFFFFFLL;   // case値の32ビットのビット列
            if (case_values.count(bits)) {
                throw std::string("compiler error: duplicate case value at ")
                      + loc_to_string(child->loc);
            }
            case_values.insert(bits);
            child->ival = v;   // コード生成器が参照できるよう畳み込み結果を保存する
        }
        // default節: 重複は不可
        else if (child->kind == ND_DEFAULT) {
            if (has_default) {
                throw std::string("compiler error: multiple default labels at ")
                      + loc_to_string(child->loc);
            }
            has_default = true;
        }
        // それ以外は通常の文として検査する
        else {
            this->analyze_stmt(child);
        }
    }

    this->scopes_.pop_back();
    this->switch_depth_--;
}

// ローカル変数宣言を検査し，現在のスコープに登録する
void Analyzer::analyze_local_decl(node_t *decl) {
    // 同一スコープ内での二重宣言はエラー (外側スコープの同名はシャドーイングとして許容)
    if (this->scopes_.back().count(decl->sval)) {
        throw std::string("compiler error: redefinition of '") + decl->sval
              + "' at " + loc_to_string(decl->loc);
    }

    // ハードウェア変数と同名のローカル変数は宣言できない (I/Oレジスタを上書きしないように禁止する)
    const symbol_t *shadowed = this->lookup_symbol(decl->sval);
    if (shadowed != nullptr && shadowed->location == LOC_REGISTER) {
        throw std::string("compiler error: cannot redeclare hardware register '") + decl->sval
              + "' at " + loc_to_string(decl->loc);
    }

    if (decl->type.is_const) {
        // const変数: 初期化子を定数式として計算して値を持たせる (メモリ番地は割り当てない)
        // 登録より前に計算するため，初期化子中の同名の参照は外側のスコープの変数を指す
        symbol_t *sym = this->register_const_var(decl);
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else if (decl->type.base == BASE_STRUCT) {
        // 構造体変数(配列宣言含む): 初期化子は非対応のため，メンバ構成に基づくアドレス確保のみ行う
        symbol_t *sym = this->register_struct_var(decl, LOC_LOCAL);
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else if (decl->type.is_array) {
        // 配列の要素数を確定させる (文字列リテラルの長さ，または定数式)
        this->resolve_decl_type(decl);
        // アドレスを割り当てて登録する
        symbol_t *sym = new symbol_t{decl->sval, decl->type, LOC_LOCAL, this->next_addr_, true, true};
        this->next_addr_ += Analyzer::calc_array_words(decl->type) * 4;
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else {
        // スカラー変数: 初期化式があれば先に検査する (登録より前に行い，自己参照 int x = x; では外側のxを参照させる)
        if (!decl->children.empty()) {
            this->analyze_expr(decl->children[0]);
            // void関数の戻り値(値を持たない)で初期化することはできない
            if (decl->children[0]->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot initialize with void value at ")
                      + loc_to_string(decl->loc);
            }
        }
        // メモリ番地を割り当てて登録する (ローカルも静的割り当てで固定番地)
        symbol_t *sym = new symbol_t{decl->sval, decl->type, LOC_LOCAL, this->next_addr_, true, true};
        this->next_addr_ += 4;
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;   // 宣言ノード自身もシンボルを指す
    }
}

// scan以外の組み込み関数の引数をチェックする(print/streq/strcopy)
// scanは構文上，対象を識別子1個(TK_IDENT)に限定しているため構造体メンバ配列(arr[i].name)が
// 現れず，書き込み可能性・最小サイズ等の固有要件も持つため，この共通検査ではなく専用の検査を別途行う
void Analyzer::check_char_array_operand(node_t *target, const std::string &builtin_name) {
    this->analyze_expr(target);
    // char型の配列でない場合はエラー
    if (!target->type.is_array || target->type.base != BASE_CHAR) {
        throw std::string("compiler error: ") + builtin_name + " requires a char array at "
              + loc_to_string(target->loc);
    }
    // 関数の配列引数(サイズ不明，array_size==0)の場合はエラー
    if (target->type.array_size == 0) {
        throw std::string("compiler error: ") + builtin_name
              + " does not support array parameters (size unknown) at " + loc_to_string(target->loc);
    }
    // 構造体配列要素のメンバ配列(arr[i].name)は実行時アドレス計算になり，コード生成が前提とする
    // 「コンパイル時に確定したアドレス」と相容れないためエラー
    if (target->kind == ND_MEMBER_ACCESS && target->children[0]->kind == ND_ARRAY_ACCESS) {
        throw std::string("compiler error: ") + builtin_name + " does not support an array member of "
              "a struct array element at " + loc_to_string(target->loc);
    }
}

// 演算の対象(名前解決・型注釈済み)がスカラーであることを検査する
// 配列・構造体を丸ごと読み書きするコード生成の仕組みは無く，そのまま通すと先頭ワードだけを
// 読み書きするコードになる(構造体は内部エラーになる)ため，意味解析の段階でエラーにする
void Analyzer::check_scalar_operand(const node_t *target, const std::string &operation) {
    if (target->type.is_array || target->type.base == BASE_STRUCT) {
        throw std::string("compiler error: ") + operation + " is not supported for array or struct at "
              + loc_to_string(target->loc);
    }
}

// 式を検査し，名前解決と型注釈を行う
// ノード種別ごとに固有の検査を行い，子を持つノードは子へ再帰する．
//   リテラル: 末端なので何もしない
//   変数参照・代入・インクリメント/デクリメント・関数呼び出し: それぞれ固有の検査を行う
//   二項演算・三項演算など(default): 固有の検査はなく，子を再帰検査するだけ
// 式文(a + b; のような文)もanalyze_stmtからこの関数で検査される
void Analyzer::analyze_expr(node_t *expr) {
    switch (expr->kind) {
        // リテラル: 検査は不要だが，後段のコード生成のため型を注釈する
        case ND_INT_LIT:
            // 整数リテラルはintまたはunsigned int
            expr->type = type_t{BASE_INT, is_signed_literal(expr->ival)};
            return;
        case ND_CHAR_LIT:
            expr->type = type_t{BASE_CHAR, true};   // 文字リテラルはchar(符号付き)
            return;

        // 文字列リテラル: 匿名のグローバルchar配列としてメモリを確保する
        case ND_STRING_LIT: {
            // サイズは文字列長 + 1(ヌル終端)
            type_t str_type = {BASE_CHAR, true, true, static_cast<int>(expr->sval.size()) + 1};
            symbol_t *sym = new symbol_t{"", str_type, LOC_GLOBAL, this->next_addr_, true, false};
            this->next_addr_ += Analyzer::calc_array_words(str_type) * 4;
            expr->sym = sym;
            expr->type = str_type;
            return;
        }

        // sizeof: 型のバイト数をコンパイル時に確定するintリテラルとして扱う (実行時命令は生成しない)
        case ND_SIZEOF: {
            int size;
            if (expr->children.empty()) {
                // sizeof(型名): パーサがexpr->typeに型を格納済み
                size = this->type_size_bytes(expr->type);
            } else {
                // sizeof(変数名): 値ではなく型だけが必要なのでND_VARのみ許可する
                // (analyze_exprではなくlookup_symbolで直接型を取得する．readable=falseの
                //  書き込み専用ハードウェア変数(LED等)もsizeofの対象になり得るため)
                node_t *inner = expr->children[0];
                if (inner->kind != ND_VAR) {
                    throw std::string("compiler error: sizeof argument must be a type name or "
                                       "variable name at ") + loc_to_string(inner->loc);
                }
                const symbol_t *sym = this->lookup_symbol(inner->sval);
                if (sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + inner->sval + "' at " + loc_to_string(inner->loc);
                }
                inner->sym  = sym;
                inner->type = sym->type;
                size = this->type_size_bytes(sym->type);
            }
            expr->ival = size;
            expr->type = type_t{BASE_INT, true};   // sizeofの結果はint
            return;
        }

        // 変数参照: 名前を解決し，読み取り可能か確認する
        case ND_VAR: {
            const symbol_t *sym = this->lookup_symbol(expr->sval);
            if (sym == nullptr) {
                throw std::string("compiler error: use of undeclared identifier '")
                      + expr->sval + "' at " + loc_to_string(expr->loc);
            }
            if (!sym->readable) {
                throw std::string("compiler error: '") + expr->sval
                      + "' is not readable at " + loc_to_string(expr->loc);
            }
            // const変数はメモリを持たないため，参照そのものを値の整数リテラルに置き換える
            // (式の中での符号の扱いが変わらないよう，リテラルの型は宣言した型のままにする)
            if (sym->location == LOC_CONST) {
                expr->kind = ND_INT_LIT;
                expr->ival = Analyzer::const_symbol_value(sym);
                expr->type = sym->type;
                expr->type.is_const = false;
                return;
            }
            expr->sym  = sym;        // 名前解決の結果を結びつける
            expr->type = sym->type;  // 型を注釈する
            return;
        }

        // 構造体メンバアクセス(a.b)の意味解析．最終的に確定させる情報はメンバの型(expr->type)と
        // 読み書き先(expr->sym)の2つ．基底(a)がメンバの番地をコンパイル時定数にできるかどうかで
        // 経路が2つに分かれる(単一の構造体変数なら定数，構造体配列の要素なら添字が実行時の値のため不定)
        case ND_MEMBER_ACCESS: {
            node_t *base = expr->children[0];   // メンバの前についている構造体変数または構造体配列要素
            const symbol_t *base_sym;           // 基底(構造体変数または構造体配列全体)のシンボル

            if (base->kind == ND_ARRAY_ACCESS) {
                // (2) 構造体配列の要素へのメンバアクセス: arr[i].member
                base_sym = this->lookup_symbol(base->sval);   // 配列全体を名前解決する
                if (base_sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + base->sval + "' at " + loc_to_string(base->loc);
                }
                // 構造体の配列でない変数へのarr[i].member形式のアクセスは禁止する
                if (!base_sym->type.is_array || base_sym->type.base != BASE_STRUCT) {
                    throw std::string("compiler error: '") + base->sval
                          + "' is not an array of struct at " + loc_to_string(base->loc);
                }
                base->sym  = base_sym;         // 配列全体のシンボルを結びつける
                base->type = base_sym->type;   // 型を注釈する
                // 添字(実行時に評価される)を検査する
                node_t *index_expr = base->children[0];
                this->analyze_expr(index_expr);
                // void値(戻り値のない関数呼び出し)は添字に使えない
                if (index_expr->type.base == BASE_VOID) {
                    throw std::string("compiler error: cannot use void value in expression at ")
                          + loc_to_string(index_expr->loc);
                }
            } else {
                // (1) 単一の構造体変数へのメンバアクセス: entry.member
                base_sym = this->lookup_symbol(base->sval);   // 構造体変数自体を名前解決する
                if (base_sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + base->sval + "' at " + loc_to_string(base->loc);
                }
                // 構造体型でない変数へのメンバアクセス(例: intの変数にx.yと書く)は禁止する
                if (base_sym->type.base != BASE_STRUCT) {
                    throw std::string("compiler error: '") + base->sval
                          + "' is not a struct at " + loc_to_string(base->loc);
                }
                base->sym  = base_sym;         // 名前解決の結果を結びつける
                base->type = base_sym->type;   // 型を注釈する
            }

            // 構造体定義からメンバ名が一致するものを探す
            const struct_def_t &def = this->struct_defs_.at(base_sym->type.struct_name);
            const struct_member_t *member = nullptr;
            for (const struct_member_t &m : def.members) {
                if (m.name == expr->sval) { member = &m; break; }
            }
            // 定義に存在しないメンバ名を指定した場合はエラー
            if (member == nullptr) {
                throw std::string("compiler error: struct '") + base_sym->type.struct_name
                      + "' has no member '" + expr->sval + "' at " + loc_to_string(expr->loc);
            }

            if (base->kind == ND_ARRAY_ACCESS) {
                // (2) 配列全体のシンボルとメンバオフセット(ワード)を注釈する (番地はコード生成側で実行時計算する)
                expr->sym  = base_sym;
                expr->ival = member->offset_words;
                expr->type = member->type;
            } else {
                // (1) 構造体変数の番地にメンバのオフセットを加えた番地を持つシンボルを合成する
                // (置き場所(location)・読み書き可否は構造体変数自身のものをそのまま引き継ぐ)
                symbol_t *sym = new symbol_t{
                    base_sym->name + "." + expr->sval,             // エラーメッセージ表示用の名前(例: "p.x")
                    member->type,                                  // メンバの型 (スカラーまたは固定長配列)
                    base_sym->location,
                    base_sym->address + member->offset_words * 4,  // 番地 = 構造体変数の番地 + メンバのオフセット
                    base_sym->readable,
                    base_sym->writable,
                };
                expr->sym  = sym;
                expr->type = member->type;
            }
            return;
        }

        // 代入: 左辺は書き込み可能なスカラーの変数・構造体メンバまたは配列要素でなければならない
        case ND_ASSIGN: {
            node_t *lhs = expr->children[0];   // 代入先(左辺)
            // 左辺を名前解決する
            if (lhs->kind == ND_VAR) {
                // 変数はanalyze_exprを通さず直接名前解決する
                // (analyze_exprの変数参照は値を読み出す側の検査であり，const変数を値のリテラルに置き換え，
                //  読み取り不可の変数をエラーにする．書き込み先にその検査を当てると誤るが，
                //  読み出し側では必要な振る舞いのため，analyze_expr自体は変えられない)
                const symbol_t *sym = this->lookup_symbol(lhs->sval);   // 左辺の変数のシンボル
                // 宣言されていない変数の場合
                if (sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + lhs->sval + "' at " + loc_to_string(lhs->loc);
                }
                // 名前解決の結果と型を左辺に注釈する
                lhs->sym  = sym;
                lhs->type = sym->type;
            } else if (lhs->kind == ND_ARRAY_ACCESS || lhs->kind == ND_MEMBER_ACCESS) {
                // 配列要素・構造体メンバはそれぞれのアクセスの検査に名前解決させる
                this->analyze_expr(lhs);
            } else {
                // 変数・配列要素・構造体メンバ以外(リテラルや式の結果)には代入できない
                throw std::string("compiler error: left side of assignment must be a variable at ")
                      + loc_to_string(expr->loc);
            }
            // 配列・構造体そのものへの代入を禁止する
            Analyzer::check_scalar_operand(lhs, "assignment");
            // 書き込みできない左辺には代入できない
            if (!lhs->sym->writable) {
                throw std::string("compiler error: '") + lhs->sym->name
                      + "' is not writable at " + loc_to_string(lhs->loc);
            }
            // 複合代入(+=等)は左辺を読みもするので，読み取り可能でもなければならない
            if (expr->sval != "=" && !lhs->sym->readable) {
                throw std::string("compiler error: '") + lhs->sym->name
                      + "' is not readable at " + loc_to_string(lhs->loc);
            }
            this->analyze_expr(expr->children[1]);   // 右辺を検査する
            // void関数の戻り値(値を持たない)を代入することはできない
            if (expr->children[1]->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot assign void value at ")
                      + loc_to_string(expr->loc);
            }
            // 代入式の値の型は左辺の型とする
            expr->type = lhs->type;
            return;
        }

        // インクリメント・デクリメント: 対象は読み書き両方可能なスカラーの変数または構造体メンバでなければならない
        case ND_UNOP:
        case ND_POST_UNOP:
            if (expr->sval == "++" || expr->sval == "--") {
                node_t *operand = expr->children[0];
                const symbol_t *sym;
                if (operand->kind == ND_VAR) {
                    // 普通の変数: 名前解決する
                    sym = this->lookup_symbol(operand->sval);
                    if (sym == nullptr) {
                        throw std::string("compiler error: use of undeclared identifier '")
                              + operand->sval + "' at " + loc_to_string(operand->loc);
                    }
                    operand->sym  = sym;
                    operand->type = sym->type;
                } else if (operand->kind == ND_MEMBER_ACCESS) {
                    // 構造体配列要素のメンバ(arr[i].member)への++/--は非対応
                    // (実行時に計算したアドレスへの++/--は追加のレジスタ計算が必要になるため．
                    //  後置は既にparse_postfixで弾いているが，前置(++arr[i].member)はここでしか検出できない)
                    if (operand->children[0]->kind == ND_ARRAY_ACCESS) {
                        throw std::string("compiler error: '++'/'--' on array element is not supported at ")
                              + loc_to_string(expr->loc);
                    }
                    // 構造体メンバ: メンバアクセスの検査(analyze_exprのND_MEMBER_ACCESSケース)に解決させる
                    this->analyze_expr(operand);
                    sym = operand->sym;
                } else {
                    // それ以外(配列要素・リテラル・式の結果等)には++/--を適用できない
                    throw std::string("compiler error: operand of '") + expr->sval
                          + "' must be a variable at " + loc_to_string(expr->loc);
                }
                Analyzer::check_scalar_operand(operand, "'" + expr->sval + "'");
                if (!sym->readable || !sym->writable) {
                    throw std::string("compiler error: '") + sym->name
                          + "' is not readable and writable at " + loc_to_string(expr->loc);
                }
                expr->type = sym->type;
                return;
            }
            // その他の前置単項演算子(-, +, !, ~): 子を検査し，void値の使用を禁止する
            this->analyze_expr(expr->children[0]);
            if (expr->children[0]->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot use void value in expression at ")
                      + loc_to_string(expr->loc);
            }
            // !の結果は0/1のint，それ以外はオペランドを整数昇格した型
            expr->type = type_t{BASE_INT, expr->sval == "!" || is_promoted_signed(expr->children[0]->type)};
            return;

        // 関数呼び出し: 関数の定義確認・引数数の検証・各引数式の検査・戻り値型の設定
        case ND_CALL: {
            auto it = this->func_names_.find(expr->sval);
            if (it == this->func_names_.end()) {
                throw std::string("compiler error: call to undefined function '")
                      + expr->sval + "' at " + loc_to_string(expr->loc);
            }
            // 呼び出しグラフに記録する (再帰・ネスト段数の検査用)
            this->call_graph_[this->current_function_].insert(expr->sval);
            // 引数の数がパラメータの数と一致するか検証する
            const auto &params = this->func_params_[expr->sval];
            if (expr->children.size() != params.size()) {
                throw std::string("compiler error: function '") + expr->sval + "' expects "
                      + std::to_string(params.size()) + " argument(s) but got "
                      + std::to_string(expr->children.size())
                      + " at " + loc_to_string(expr->loc);
            }
            // 各引数式を検査する
            for (size_t i = 0; i < expr->children.size(); i++) {
                this->analyze_expr(expr->children[i]);
                // void値(戻り値のない関数呼び出し)は引数に使えない
                if (expr->children[i]->type.base == BASE_VOID) {
                    throw std::string("compiler error: cannot use void value in expression at ")
                          + loc_to_string(expr->children[i]->loc);
                }
                // 構造体はフルコピーの仕組みが無いため，引数として渡すこと自体を禁止する
                if (expr->children[i]->type.base == BASE_STRUCT) {
                    throw std::string("compiler error: struct cannot be passed as a function argument at ")
                          + loc_to_string(expr->children[i]->loc);
                }
                // 配列パラメータには配列変数(構造体メンバ配列を含む)または文字列リテラルを，
                // スカラーパラメータにはスカラー式を渡す
                node_t *arg = expr->children[i];
                if (params[i]->type.is_array) {
                    const bool is_array_designator =
                        arg->kind == ND_VAR || arg->kind == ND_STRING_LIT || arg->kind == ND_MEMBER_ACCESS;
                    // is_arrayの判定はarg->typeを見る(構造体配列要素のメンバの場合，arg->symは
                    // メンバ自身ではなく配列全体を指すため，arg->sym->type.is_arrayでは判定できない)
                    if (!is_array_designator || !arg->type.is_array) {
                        throw std::string("compiler error: argument for array parameter '")
                              + params[i]->name + "' must be an array variable or string literal at "
                              + loc_to_string(arg->loc);
                    }
                    // 構造体配列要素のメンバ配列(arr[i].name)は実行時アドレス計算になり，
                    // 関数呼び出し規約(呼び出し元がコンパイル時アドレスを直接書き込む方式)と
                    // 相容れないため，引数として渡すことを禁止する
                    if (arg->kind == ND_MEMBER_ACCESS && arg->children[0]->kind == ND_ARRAY_ACCESS) {
                        throw std::string("compiler error: array member of a struct array element cannot be "
                                           "passed as a function argument at ") + loc_to_string(arg->loc);
                    }
                    // 要素型(符号を含む)が異なる配列の場合 (呼び出し先は自身の要素型で読み書きするため，値を取り違える)
                    if (arg->type.base != params[i]->type.base || arg->type.is_signed != params[i]->type.is_signed) {
                        throw std::string("compiler error: array element type mismatch for parameter '")
                              + params[i]->name + "' at " + loc_to_string(arg->loc);
                    }
                } else {
                    if ((arg->kind == ND_VAR || arg->kind == ND_MEMBER_ACCESS) && arg->type.is_array) {
                        // 構造体配列要素のメンバ配列(arr[i].name)はarg->symが配列全体("arr")を
                        // 指すため，エラーメッセージ表示用に "配列名[].メンバ名" の形に組み立てる
                        const std::string arg_name =
                            (arg->kind == ND_MEMBER_ACCESS && arg->children[0]->kind == ND_ARRAY_ACCESS)
                                ? arg->sym->name + "[]." + arg->sval
                                : arg->sym->name;
                        throw std::string("compiler error: cannot pass array '")
                              + arg_name + "' to scalar parameter '"
                              + params[i]->name + "' at " + loc_to_string(arg->loc);
                    }
                    // スカラー引数は型が異なっても受け付け，パラメータの型で格納する
                    // (値の変換規則が決まっているため，型の一致は検査しない)
                }
            }
            expr->type = it->second;
            return;
        }

        // 組み込み関数print: char配列(直接配列)のみ対応．ヌル終端まで出力する
        case ND_PRINT: {
            node_t *target = expr->children[0];
            this->check_char_array_operand(target, "print");
            // printは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 組み込み関数streq: 2つのchar配列(直接配列)の内容が一致するか比較する
        case ND_STREQ: {
            node_t *lhs = expr->children[0];
            node_t *rhs = expr->children[1];
            this->check_char_array_operand(lhs, "streq");
            this->check_char_array_operand(rhs, "streq");
            // streqは0/1のint値を返す(if文の条件式等にそのまま使える)
            expr->type = type_t{BASE_INT, true};
            return;
        }

        // 組み込み関数strcopy: 第2引数(src)の内容を第1引数(dst)へヌル終端付きでコピーする
        case ND_STRCOPY: {
            node_t *dst = expr->children[0];
            node_t *src = expr->children[1];
            this->check_char_array_operand(dst, "strcopy");
            this->check_char_array_operand(src, "strcopy");
            if (!dst->sym->writable) {
                throw std::string("compiler error: '") + dst->sym->name
                      + "' is not writable at " + loc_to_string(dst->loc);
            }
            // strcopyは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 組み込み関数scan: char配列(直接配列，2要素以上)のみ対応．改行までの1行をヌル終端付きで格納する
        // 書き込み可能性・最小サイズの検査が必要なためcheck_char_array_operandは使わず専用の検査を行う
        case ND_SCAN: {
            node_t *target = expr->children[0];   // 格納先配列 (parserがND_VARで構築)
            const symbol_t *sym = this->lookup_symbol(target->sval);
            if (sym == nullptr) {
                throw std::string("compiler error: use of undeclared identifier '")
                      + target->sval + "' at " + loc_to_string(target->loc);
            }
            if (!sym->writable) {
                throw std::string("compiler error: '") + target->sval
                      + "' is not writable at " + loc_to_string(target->loc);
            }
            target->sym  = sym;        // 名前解決の結果を結びつける
            target->type = sym->type;
            if (!sym->type.is_array || sym->type.base != BASE_CHAR) {
                throw std::string("compiler error: scan requires a char array at ")
                      + loc_to_string(target->loc);
            }
            if (sym->type.array_size == 0) {
                throw std::string("compiler error: scan does not support array parameters (size unknown) at ")
                      + loc_to_string(target->loc);
            }
            if (sym->type.array_size < 2) {
                throw std::string("compiler error: scan target array must have at least 2 elements "
                                   "(1 for content plus 1 for null terminator) at ")
                      + loc_to_string(target->loc);
            }
            // scanは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 配列要素アクセス: 配列(構造体メンバ配列を含む)の名前解決とインデックス式の検査を行う
        // 通常の配列変数はsval(変数名)で解決し，構造体メンバ配列はchildren[1](ND_MEMBER_ACCESS)で解決する
        case ND_ARRAY_ACCESS: {
            const symbol_t *sym;
            type_t elem_type;   // 配列要素自身の型(is_arrayかどうかの判定にも使う)
            if (expr->children.size() == 2) {
                // 構造体メンバ配列: children[1]のメンバアクセスを検査させる．
                // designator->symは通常のメンバなら「メンバ自身」，構造体配列要素のメンバなら
                // 「配列全体」を指す(コード生成でのアドレス計算用)ため，配列判定・要素型には
                // designator->type(常にメンバ自身の型)を使う
                node_t *designator = expr->children[1];
                this->analyze_expr(designator);
                sym = designator->sym;
                elem_type = designator->type;
            } else {
                // 通常の配列変数: svalに入っている変数名で名前解決する
                sym = this->lookup_symbol(expr->sval);
                if (sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + expr->sval + "' at " + loc_to_string(expr->loc);
                }
                elem_type = sym->type;
                // 構造体配列は，要素(構造体1個分)を直接使うことができない(メンバアクセス経由でのみ使える)．
                // ND_MEMBER_ACCESSの基底として使われる場合はそちらが先にこのcaseを経由せず処理するため，
                // ここに到達するのは単独で使われた場合(x = arr[i];等)であり，常にエラーにしてよい
                if (elem_type.is_array && elem_type.base == BASE_STRUCT) {
                    throw std::string("compiler error: struct array element must be accessed via a "
                                       "member (e.g. arr[i].member) at ") + loc_to_string(expr->loc);
                }
            }
            if (!elem_type.is_array) {
                const std::string name = expr->sval.empty() ? sym->name : expr->sval;
                throw std::string("compiler error: '") + name
                      + "' is not an array at " + loc_to_string(expr->loc);
            }
            expr->sym = sym;
            // 要素の型は配列のbase型(スカラー)
            expr->type = {elem_type.base, elem_type.is_signed};
            // インデックス式を検査する
            node_t *index_expr = expr->children[0];
            this->analyze_expr(index_expr);
            // void値(戻り値のない関数呼び出し)は配列インデックスに使えない
            if (index_expr->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot use void value in expression at ")
                      + loc_to_string(index_expr->loc);
            }
            return;
        }

        // 二項演算: 両辺を検査し，どちらかがvoid値(戻り値のない関数呼び出し)なら使用を禁止する
        case ND_BINOP: {
            this->analyze_expr(expr->children[0]);
            this->analyze_expr(expr->children[1]);
            if (expr->children[0]->type.base == BASE_VOID || expr->children[1]->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot use void value in expression at ")
                      + loc_to_string(expr->loc);
            }
            const std::string &op = expr->sval;   // 演算子
            // 比較・論理演算の結果は0/1のint
            const bool is_boolean = op == "==" || op == "!=" || op == "<" || op == ">"
                                 || op == "<=" || op == ">=" || op == "&&" || op == "||";
            // それ以外は，演算の符号(符号付きで演算するならint，符号なしならunsigned int)
            const bool is_signed =
                is_boolean || is_signed_operation(op, expr->children[0]->type, expr->children[1]->type);
            expr->type = type_t{BASE_INT, is_signed};
            return;
        }

        // 三項演算 a ? b : c: 条件・両分岐を検査し，いずれかがvoid値なら使用を禁止する
        case ND_TERNARY:
            this->analyze_expr(expr->children[0]);
            this->analyze_expr(expr->children[1]);
            this->analyze_expr(expr->children[2]);
            if (expr->children[0]->type.base == BASE_VOID ||
                expr->children[1]->type.base == BASE_VOID ||
                expr->children[2]->type.base == BASE_VOID) {
                throw std::string("compiler error: cannot use void value in expression at ")
                      + loc_to_string(expr->loc);
            }
            // 結果は両方の分岐の値が昇格後intならint，そうでなければunsigned int
            expr->type = type_t{BASE_INT, is_promoted_signed(expr->children[1]->type)
                                          && is_promoted_signed(expr->children[2]->type)};
            return;

        // 到達しない (式ノードの全種類は上記いずれかのcaseで処理される)．
        // 将来式ノードを追加した際に検査漏れとなるのを防ぐため，未対応として即エラーにする
        default:
            throw std::string("compiler error: unsupported expression node kind at ")
                  + loc_to_string(expr->loc);
    }
}

// 名前からシンボルを探す (内側のローカルスコープから順に，最後にグローバル・ハードウェア変数)
const symbol_t *Analyzer::lookup_symbol(const std::string &name) const {
    // ローカルスコープを内側から外側へ探す
    for (auto it = this->scopes_.rbegin(); it != this->scopes_.rend(); ++it) {
        const auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    // グローバル変数・ハードウェア変数を探す
    const auto found = this->symbols_.find(name);
    if (found != this->symbols_.end()) return found->second;
    return nullptr;
}
