// 全二項演算子の確認 (グローバルは定数畳み込み，ローカルは命令生成)
// グローバル: 各演算子を畳み込みで初期化する
int g_add = 1 + 2;       // 3
int g_sub = 5 - 2;       // 3
int g_mul = 2 * 3;       // 6
int g_div = 17 / 5;      // 3
int g_mod = 17 % 5;      // 2
int g_and = 6 & 3;       // 2
int g_or  = 6 | 1;       // 7
int g_xor = 6 ^ 3;       // 5
int g_shl = 1 << 4;      // 16
int g_shr = 64 >> 2;     // 16
int g_nest = 1 + 2 * 3;  // グローバルの初期化式はコンパイル時に畳み込まれ 7 になる

void main(void) {
    // ローカル: 各演算子を命令で評価する
    int l_add = 1 + 2;
    int l_sub = 5 - 2;
    int l_mul = 2 * 3;
    int l_div = 17 / 5;
    int l_mod = 17 % 5;
    int l_and = 6 & 3;
    int l_or  = 6 | 1;
    int l_xor = 6 ^ 3;
    int l_shl = 1 << 4;
    int l_shr = 64 >> 2;
    int l_nest = 1 + 2 * 3;   // ネスト式 (レジスタスタックの確認)
}