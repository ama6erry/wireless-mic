package com.example.wirelessmic

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.material3.Button
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.lifecycle.lifecycleScope
import com.example.wirelessmic.ui.theme.WirelessMicTheme
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private val udpClient = UdpClient()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            WirelessMicTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    Column{
                        TextField(modifier = Modifier.padding(innerPadding), state = rememberTextFieldState(initialText = ""), label = {Text("IP Address")})
                        TextField(modifier = Modifier.padding(innerPadding), state = rememberTextFieldState(initialText = ""), label = {Text("Port")})
                        TextField(modifier = Modifier.padding(innerPadding), state = rememberTextFieldState(initialText = ""), label = {Text("Message")})
                        Button(
                            onClick = {lifecycleScope.launch {
                                val result = udpClient.sendUdpMessage(
                                    message = "Hello", serverIp = "10.0.2.2", port = 9800
                                )
                            }},
                            modifier = Modifier.padding(innerPadding)
                        ){
                            Text("Send UDP")
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun Greeting(name: String, modifier: Modifier = Modifier) {
    Text(
        text = "Hello $name!",
        modifier = modifier
    )
}

@Preview(showBackground = true)
@Composable
fun GreetingPreview() {
    WirelessMicTheme {
        Greeting("Android")
    }
}