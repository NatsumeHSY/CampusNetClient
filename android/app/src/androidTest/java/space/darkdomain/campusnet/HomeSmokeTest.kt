package space.darkdomain.campusnet

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import org.junit.Rule
import org.junit.Test

class HomeSmokeTest {
    @get:Rule val compose = createComposeRule()
    @Test fun showsTitle() {
        compose.setContent { CampusApp(true, BatteryState.OK, EnvironmentKind.UNKNOWN) }
        compose.onNodeWithText("广航海校园网自动登录").assertIsDisplayed()
    }
}
