`wss.c` is a proof of concept that uses idle page tracking from Linux 4.3+, for a page-based Working Set Size (WSS) estimation. This version snapshots the entire system's idle page flags. This tool is written for x86_64 and default page size only. 
 
- To Compile:`gcc -o wss wss.c`
- Usage: `wss <PID> <duration(in sec)>`
    For Example, `wss 4670 1` will track the WSS of process `4670` and the sample duration is 1 second. This tool will log the following outputs.
    - `Est(s)`: Estimated WSS measurement duration: this accounts for delays with setting and reading pagemap data, which inflates the intended sleep duration.
    - `Ref(MB)`: Referenced (Mbytes) during the specified duration. This is the working set size metric.
    - `Active_Pages`:	Number of pages active within the specified duration. 
    - `epoch`: Epoch elapsed after the tool runs
    - `used  free  buff  cach`: These four columns represent the memory used by the application during that epoch
    - `used  free`: These two columns indicates the swap usage during that epoch
    - `in   out`:	The swap in and out rate; if paging happens
    - `memory process`: The name of teh process that consumes tha maximum memoru usage during that epoch