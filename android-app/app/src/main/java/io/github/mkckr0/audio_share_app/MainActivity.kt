/*
 *    Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package io.github.mkckr0.audio_share_app

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import io.github.mkckr0.audio_share_app.model.Asset
import io.github.mkckr0.audio_share_app.service.PlaybackService
import io.github.mkckr0.audio_share_app.ui.screen.MainScreen
import io.github.mkckr0.audio_share_app.ui.theme.AppTheme

class MainActivity : ComponentActivity() {

    private lateinit var mediaControllerFuture: ListenableFuture<MediaController>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        enableEdgeToEdge()
        setContent {
            AppTheme {
                MainScreen()
            }
        }

        // create notification channel
        if (Build.VERSION.SDK_INT > Build.VERSION_CODES.O) {
            val notificationManager =
                getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            val channel = NotificationChannel(
                Channel.UPDATE.id,
                Channel.UPDATE.title,
                Channel.UPDATE.importance
            )
            notificationManager.createNotificationChannel(channel)
        }

        // request permissions
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 0)
        }

        val sessionToken =
            SessionToken(application, ComponentName(application, PlaybackService::class.java))
        mediaControllerFuture = MediaController.Builder(application, sessionToken).buildAsync()
    }

    override fun onDestroy() {
        super.onDestroy()
        MediaController.releaseFuture(mediaControllerFuture)
    }

    fun postToMediaController(task: (mediaController: MediaController) -> Unit) {
        mediaControllerFuture.addListener({
            task(mediaControllerFuture.get())
        }, ContextCompat.getMainExecutor(this))
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)

        // tap update notification to start downloading
        if (intent.getStringExtra("action") == "update") {

            val apkAsset = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra("apkAsset", Asset::class.java)!!
            } else {
                @Suppress("DEPRECATION") intent.getParcelableExtra("apkAsset")!!
            }

            // download via Browser
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(apkAsset.browserDownloadUrl)))

            /*
            // download via DownloadManager
            val downloadManager = getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
            val preferences = PreferenceManager.getDefaultSharedPreferences(application)
            var downloadId = preferences.getLong("update_download_id", -1)
            if (downloadManager.getUriForDownloadedFile(downloadId) != null) {
                val cursor =
                    downloadManager.query(DownloadManager.Query().setFilterById(downloadId))
                val index = cursor.getColumnIndex(DownloadManager.COLUMN_TITLE)
                if (cursor.moveToFirst() && cursor.getString(index) == apkAsset.name) {
                    @Suppress("NAME_SHADOWING") val intent = Intent(Intent.ACTION_VIEW)
                    intent.setDataAndType(
                        downloadManager.getUriForDownloadedFile(downloadId),
                        downloadManager.getMimeTypeForDownloadedFile(downloadId)
                    )
                    intent.setFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    startActivity(intent)
                }
                cursor.close()
            } else {
                downloadManager.remove(downloadId)
                preferences.edit().remove("update_download_id").apply()

                // Enqueue a new download
                val request =
                    DownloadManager.Request(Uri.parse(apkAsset.browserDownloadUrl)).apply {
                        setTitle(apkAsset.name)
                        setDestinationInExternalFilesDir(
                            this@MainActivity, null, apkAsset.name
                        )
                        setNotificationVisibility(
                            DownloadManager.Request.VISIBILITY_VISIBLE.or(
                                DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED
                            )
                        )
                    }
                downloadId = downloadManager.enqueue(request)
                preferences.edit().putLong("update_download_id", downloadId).apply()
                Log.d(tag, "download ${apkAsset.name} $downloadId")
            }
            // */
        }
    }

}
