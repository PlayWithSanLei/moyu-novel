package app.moyureader

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test

class HomeCopyTest {
    @Test
    fun placeholderTitleIsTheAppName() {
        assertEquals("摸鱼阅读器", HOME_TITLE)
        assertEquals("home", HOME_ROUTE)
    }
}
