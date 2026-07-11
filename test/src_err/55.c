// 意味解析エラー: 配列変数をスカラーパラメータに渡すことはできない (異常系)
int f(int x) {
    return x;
}
void main(void) {
    int arr[4];
    int y = f(arr);
}
