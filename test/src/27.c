// 二項演算の右辺(reg+1)に引数ありの関数呼び出しが来ても左辺(reg)の値を破壊しないことの確認
int f(int p) {
    return p + 1;
}
void main(void) {
    int a;
    int b;
    int x;
    a = 10;
    b = 20;
    x = a + f(b);
}
