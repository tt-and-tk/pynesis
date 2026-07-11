// 意味解析エラー: scanは配列パラメータ(サイズ不明)には対応しない (異常系)
void read_line(char arr[]) {
    scan(arr);
}
void main(void) {
    char buf[4];
    read_line(buf);
}
