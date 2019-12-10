# CompNet.Lab3 Report

> Can Su 1700012779

[TOC]

## Writing Task

- How do you estimate the bandwidth and the delay of a path?

    When a HTTP/2 connection is established, we start to send HTTP/2 PING frames to the server continuously. And when we receive an ACK to the PING, the **RTT~THIS_TIME~** is estimated by the interval between sending and receiving. The RTT is then estimated by the following formula
    
    ​	**RTT~NEW~ = ( 4 * RTT~OLD~ + RTT~THIS_TIME~ ) / 5**
    Similarly, the bandwidth is estimate by
    
    ​	**BANDWIDTH~THIS_TIME~ = BYTES_DOWNLOADED / ( TIME~PING_RECEIVED~ - TIME~DOWNLOAD_BEGIN~ )**
    and
    ​	**BANDWIDTH~NEW~ = ( 4 * BANDWIDTH~OLD~ + BANDWIDTH~THIS_TIME~ ) / 5**
    
- How do you assign jobs to the three paths?

    The first path initially request for the whole file. Once the response is received, we get the file size **D**. The other two paths are then set to begin downloading at **[ D / 3 ]** and **2 * [ D / 3 ]** respectively, each requesting about **[ D / 3 ]** bytes. The range of the first path is also adjusted to  **0 ~ [ D / 3 ] - 1**.
    When the fastest path (let's say path *i*) is about to finish its job, it calculates the remaining time of the other two and chooses the slower one (suppose it's path *j*). Then we issue a new request on path *i* and adjust the range of path *j* based on the equation
        **REMAINING_TIME~i~ + D~i~ / BANDWIDTH~i~ = D~j~ / BANDWIDTH~j~** and **D~i~ + D~j~ = BYTES_TO_DOWNLOAD~j~**
    The adjusting process may iterate multiple times due to the imperfection estimation of RTTs and bandwidths.
    
- What features (pipelining, eliminating tail byes, etc.) do you implement? And how do you implement them? 

    Pipelining is implemented based on HTTP/2 PING frames. We use HTTP/2 PING frames to estimate the bandwidth and therefore the remaining time to finish the download. So we can send the new request one RTT before the download finishes.

## Drawing Task

- NetEm

  - Under static network conditions:

    Each path is set with 5ms delay and 10Mbps bandwidth.

  ![static](h2o/plot/static.png)

  - Under varying bandwidth conditions:

    Bandwidth is set to vary from 5Mbps to 10Mbps. However, the download rate strangely seems to be unaffected.

  ![varius](h2o/plot/varius.png)

  - With a high-bandwidth path and two low-bandwidth paths:

    The high-bandwidth path is set with 5ms delay and 10Mbps bandwidth, whereas the low-bandwidth paths are set with 5ms delay and 7Mbps bandwidth.

  ![bandwidth](h2o/plot/bandwidth.png)

  - With a short-delay path and two long-delay paths:

    The short-delay path is set with 5ms delay and 10Mbps bandwidth, whereas the long-delay paths are set with 10ms delay and 10Mbps bandwidth.

  ![delay](h2o/plot/delay.png)

- Mahimahi

  Random traces are chosen to demonstrate the download progress:

  ![mahimahi_1](h2o/plot/mahimahi_1.png)

![mahimahi_2](h2o/plot/mahimahi_2.png)

