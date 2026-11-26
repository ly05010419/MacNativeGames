/* NSString lengths are UTF-16 units, matching nn::swkbd. Keep composed input
   intact; the UI calls this only after the input method finishes composition. */
static NSString *runtime_text_filtered(NSString *text, int maxUnits, int digitsOnly, uint32_t disabled) {
    NSMutableString *result = [NSMutableString string];
    [text enumerateSubstringsInRange:NSMakeRange(0,text.length)
        options:NSStringEnumerationByComposedCharacterSequences
        usingBlock:^(NSString *part, NSRange range, NSRange enclosing, BOOL *stop) {
        (void)range; (void)enclosing;
        for (NSUInteger i=0;i<part.length;i++) {
            unichar c=[part characterAtIndex:i]; BOOL digit=c>='0'&&c<='9';
            if ((digitsOnly&&!digit)||c<0x20||c==0x7f||
                ((disabled&(1u<<1))&&c==' ')||((disabled&(1u<<2))&&c=='@')||
                ((disabled&(1u<<3))&&c=='%')||((disabled&(1u<<4))&&c=='/')||
                ((disabled&(1u<<5))&&c=='\\')||((disabled&(1u<<6))&&digit)) return;
        }
        if (maxUnits>0&&result.length+part.length>(NSUInteger)maxUnits) { *stop=YES; return; }
        [result appendString:part];
    }];
    return result;
}
