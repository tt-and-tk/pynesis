// 意味解析エラー: printは配列パラメータ(サイズ不明)には対応しない (異常系)
void show(char arr[]) {
    print(arr);
}
void main(void) {
    char buf[4];
    show(buf);
}
