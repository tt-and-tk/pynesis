// 意味解析エラー: 三項演算子の条件式にvoid値(scanの戻り値)は使えない (異常系)
char buf[8];
void main(void) {
    int x;
    x = scan(buf) ? 1 : 2;
}
