package com.example.wimic

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {

    private var isStreaming by mutableStateOf(false)

    private val requestRecordAudioPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            isStreaming = true
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            MaterialTheme {
                var ip by remember { mutableStateOf("") }
                var port by remember { mutableStateOf("") }
                var err by remember { mutableStateOf(false) }
                Surface(modifier = Modifier.fillMaxSize()) {
                    Column(
                        modifier = Modifier.fillMaxSize(),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        TextField(
                            value = ip,
                            onValueChange = {ip = it},
                            label = {Text("IP Address")}
                        )
                        TextField(
                            value = port,
                            onValueChange = { newValue ->
                                if(newValue.all {it.isDigit() }){
                                    port = newValue
                                    err = false
                                } else {
                                    err = true
                                }
                                            },
                            label = {Text("Port")},
                            isError = err,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
                        )
                        Button(onClick = {
                            if(isStreaming){
                                stopCapture()
                                isStreaming = false
                            } else {
                                onStartStreamClicked(ip, port.toInt())
                            }

                        }) {
                            Text(if (isStreaming) "Stop streaming" else "Start Mic Stream")
                        }
                    }
                }
            }
        }
    }

    private fun onStartStreamClicked(ip : String, port : Int) {
        if (isStreaming) return

        val hasPermission = ContextCompat.checkSelfPermission(
            this, Manifest.permission.RECORD_AUDIO
        ) == PackageManager.PERMISSION_GRANTED

        if (hasPermission) {
            startCapture(ip, port)
            isStreaming = true
        } else {
            requestRecordAudioPermission.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    /**
     * Allocates the Oboe input stream and starts it. Implemented in native-lib.cpp.
     */
    external fun startCapture(ip : String, port : Int);
    external fun stopCapture();

    companion object {
        // Used to load the 'wimic' library on application startup.
        init {
            System.loadLibrary("wimic")
        }
    }
}
