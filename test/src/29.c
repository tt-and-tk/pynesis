// 比較・複合代入・配列添字それぞれで，右辺(または後続オペランド)に関数呼び出しを含んでも
// 既に評価済みの値が破壊されないことの確認
int f(int p) {
    return p + 1;
}
int arr[8];
void main(void) {
    int a;
    int b;
    int c;
    a = 5;
    b = 3;
    c = 0;
    if (a > f(b)) {   // a(5) > f(b)=4 → 真
        c = 1;
    }
    c += f(a);        // c(1) + f(5)=6 = 7
    arr[f(a)] = c;    // arr[6] = 7
}
