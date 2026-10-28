package space.darkdomain.campusnet

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.map
import java.security.KeyStore
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

private val Context.settingsStore by preferencesDataStore(name = "campus_settings")

data class SavedSettings(val carrier: Carrier = Carrier.TELECOM, val mac: String = "", val credentials: Credentials? = null, val credentialsUnreadable: Boolean = false)

class SettingsRepository(internal val context: Context) {
    private val carrier = stringPreferencesKey("carrier")
    private val credentials = stringPreferencesKey("credentials_v1")
    private val wifiMac = stringPreferencesKey("wifi_mac")
    private val keyAlias = "campusnet_credentials_v1"

    private fun getOrCreateKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(keyAlias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(keyAlias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build())
        }.generateKey()
    }

    val settings = context.settingsStore.data.map { values ->
        val encrypted = values[credentials]
        val decoded = if (encrypted == null) null else runCatching { decryptCredentials(getOrCreateKey(), encrypted) }
        SavedSettings(Carrier.fromStored(values[carrier]), values[wifiMac] ?: "", decoded?.getOrNull(), decoded?.isFailure == true)
    }

    suspend fun save(selected: Carrier, user: String, password: String, mac: String) {
        val encrypted = encryptCredentials(getOrCreateKey(), user, password)
        context.settingsStore.edit { it[carrier] = selected.name; it[credentials] = encrypted; it[wifiMac] = mac }
    }

    suspend fun clearCredentials() { context.settingsStore.edit { it.remove(credentials) } }
}
