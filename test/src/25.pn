// 配列要素読み出し時の符号拡張 (char/short，最上位ビットが立つ値)
char cbuf[2];
short sbuf[2];
void main(void) {
    cbuf[0] = 200;       // 符号付き8ビットでは -56
    int a = cbuf[0];     // -56 (符号拡張して読み出す)
    sbuf[0] = 40000;     // 符号付き16ビットでは -25536
    int b = sbuf[0];     // -25536 (符号拡張して読み出す)
}
