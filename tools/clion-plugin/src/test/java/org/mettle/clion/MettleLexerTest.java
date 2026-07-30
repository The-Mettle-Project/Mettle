package org.mettle.clion;

import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import junit.framework.TestCase;
import org.mettle.clion.lang.MettleLexer;
import org.mettle.clion.lang.MettleTypes;

import java.util.ArrayList;
import java.util.List;

/** The lexer rules that are easy to get subtly wrong. */
public class MettleLexerTest extends TestCase {

    private static List<IElementType> tokens(String text) {
        MettleLexer lexer = new MettleLexer();
        lexer.start(text);
        List<IElementType> types = new ArrayList<>();
        while (lexer.getTokenType() != null) {
            if (lexer.getTokenType() != TokenType.WHITE_SPACE) types.add(lexer.getTokenType());
            lexer.advance();
        }
        return types;
    }

    private static List<String> texts(String text) {
        MettleLexer lexer = new MettleLexer();
        lexer.start(text);
        List<String> result = new ArrayList<>();
        while (lexer.getTokenType() != null) {
            if (lexer.getTokenType() != TokenType.WHITE_SPACE) {
                result.add(text.substring(lexer.getTokenStart(), lexer.getTokenEnd()));
            }
            lexer.advance();
        }
        return result;
    }

    public void testBlockCommentsNest() {
        assertEquals(List.of(MettleTypes.BLOCK_COMMENT, MettleTypes.KW_VAR),
                tokens("/* outer /* inner */ still commented */ var"));
    }

    public void testUnterminatedBlockCommentRunsToEndOfFile() {
        assertEquals(List.of(MettleTypes.BLOCK_COMMENT), tokens("/* never closed"));
    }

    public void testRangeIsNotAFloat() {
        assertEquals(List.of(MettleTypes.INT_LITERAL, MettleTypes.DOTDOT, MettleTypes.INT_LITERAL),
                tokens("1..5"));
        assertEquals(List.of(MettleTypes.INT_LITERAL, MettleTypes.DOTDOTEQ, MettleTypes.INT_LITERAL),
                tokens("0..=n".replace("n", "9")));
    }

    public void testNumericLiterals() {
        assertEquals(List.of(MettleTypes.FLOAT_LITERAL), tokens("3.14"));
        assertEquals(List.of(MettleTypes.FLOAT_LITERAL), tokens("1e10"));
        assertEquals(List.of(MettleTypes.FLOAT_LITERAL), tokens("2.220446049250313e-16"));
        // hex digits include 'e', so 0x1E is an integer, not an exponent
        assertEquals(List.of(MettleTypes.INT_LITERAL), tokens("0x1E"));
        assertEquals(List.of(MettleTypes.INT_LITERAL), tokens("0b1010"));
        // an identifier butted against a number still lexes separately
        assertEquals(List.of(MettleTypes.INT_LITERAL, MettleTypes.IDENTIFIER), tokens("1exp"));
    }

    public void testCommentMarkersInsideStringsAreNotComments() {
        assertEquals(List.of(MettleTypes.STRING_LITERAL), tokens("\"http://example.com\""));
        assertEquals(List.of("\"a\\\"b\""), texts("\"a\\\"b\""));
    }

    public void testCharacterLiterals() {
        assertEquals(List.of(MettleTypes.CHAR_LITERAL), tokens("'A'"));
        assertEquals(List.of(MettleTypes.CHAR_LITERAL), tokens("'\\n'"));
    }

    public void testOperatorsPreferTheLongestMatch() {
        assertEquals(List.of(MettleTypes.SHL_EQ), tokens("<<="));
        assertEquals(List.of(MettleTypes.SHR), tokens(">>"));
        assertEquals(List.of(MettleTypes.ARROW), tokens("->"));
        assertEquals(List.of(MettleTypes.MINUS_EQ), tokens("-="));
        assertEquals(List.of(MettleTypes.ANDAND), tokens("&&"));
    }

    public void testKeywordsAndIdentifiers() {
        assertEquals(List.of(MettleTypes.KW_FN, MettleTypes.IDENTIFIER, MettleTypes.LPAREN,
                        MettleTypes.RPAREN, MettleTypes.ARROW, MettleTypes.TY_INT32),
                tokens("fn add() -> int32"));
        // `in` is contextual: it stays an ordinary identifier
        assertEquals(List.of(MettleTypes.IDENTIFIER), tokens("in"));
    }

    public void testEveryTokenAdvances() {
        String text = "@#$`é var";
        MettleLexer lexer = new MettleLexer();
        lexer.start(text);
        int previousEnd = -1;
        while (lexer.getTokenType() != null) {
            assertTrue("lexer must make progress", lexer.getTokenEnd() > previousEnd);
            previousEnd = lexer.getTokenEnd();
            lexer.advance();
        }
        assertEquals(text.length(), previousEnd);
    }
}
