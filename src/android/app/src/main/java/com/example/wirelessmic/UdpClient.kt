package com.example.wirelessmic

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

class UdpClient {
    suspend fun sendUdpMessage(message : String, serverIp : String, port : Int): Boolean{
        return withContext(Dispatchers.IO){
            try{
                DatagramSocket().use {socket ->
                    val bytes = message.toByteArray(Charsets.UTF_8)
                    val address = InetAddress.getByName(serverIp)
                    val packet = DatagramPacket(bytes, bytes.size, address, port)

                    socket.send(packet)
                    true
                }
            } catch (e: IOException){
                e.printStackTrace()
                false
            }
        }
    }
}