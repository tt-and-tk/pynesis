// 比較の値化・論理&&/||・三項演算子 (グローバル・ローカル変数)
int g = 0;
void main(void) {
    int a = 3;
    int b = 0;
    int x = a < b;          // 比較の値化 (0/1)
    int y = a && b;         // 論理&& (短絡)
    int z = a || b;         // 論理|| (短絡)
    int w = a < b ? a : b;  // 三項演算子
}
