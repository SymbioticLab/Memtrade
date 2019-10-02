import sys;
import os;
import math;

def process_argv(argv):
    options = {};
    while argv:
        if argv[0][0] == '-':
            options[argv[0]] = argv[1];
        argv = argv[1:];
    return options;

def parse_trace(filename, fault_count=100000, func_trace='__do_page_fault()'):
    line_count = -1;
    func_map = {};
    time_map = {};
    cdf_map = {};
    entry_func = [];
    entry_curl_count = [];
    count = 0;
    output_base_url = "./";

    with open(filename) as log:
        for line in log:
            total_token = 0;
            values = str.split(line);
            for index,token in enumerate(values):
                total_token = index;
                entry_func_found = 1;
                if token == 'us':
                    entry_func_found = 0;
                    time = (float)(values[index-1]);
                    func_name = values[index+2];
                    #print(func_name);
                    if func_name == '}':
#                        print(entry_func);
                        #print(len(entry_curl_count));
                        for i in range(len(entry_curl_count)):
                            entry_curl_count[i] = (int) (entry_curl_count[i]) - 1;
                            if entry_curl_count[i] <= 0:
                                func_name = entry_func[i];
                                entry_func.pop(i);
                                entry_curl_count.pop(i);
                                break;
                    if func_name not in time_map:
                        time_map[func_name] = [];
                    time_map[func_name].append(time);
                    if func_name == func_trace:
                        count = count + 1;
                        if count % 10000 == 0:
                            print("%0.2f%% is done, %d out of %d lines" %((count*100.0/fault_count), count, fault_count));
#            print(values);
            if entry_func_found == 1 and values[total_token] == '{':
                for i in range(len(entry_curl_count)):
                    entry_curl_count[i] = (int) (entry_curl_count[i]) + 1;
                entry_func.append(values[total_token-1]);
                entry_curl_count.append(1);
            if values[0] == '#eof':
                break;
            line_count = line_count + 1;
            if count >= fault_count:
                #print(time_map);
                break;

    f = open(output_base_url+'cdf.txt', 'w');
    for func, times in time_map.items():
        cdf_map[func]=[];
        length = len(times);
        if length < 0.4*fault_count:
            continue;
        times = sorted(times);
        for i in range(1, 101):
            idx = (int) (length * 0.01 * i - 1);
#            print(func, idx);
            cdf_map[func].append(times[idx]);
        
        f.write("\n=============\n%s\n=================\n" % (func));
        for i in range(len(cdf_map[func])):
            f.write("%f\n" % (cdf_map[func][i]));

#        print(func, "========>\n", cdf_map[func], "<========\n");      
    f.close();

def main():
    myargvs = process_argv(sys.argv);

    log_file = "/proj/gaia-PG0/hasan/ftrace_output_sample.txt";
    count = 100000;
    func = "submit_bio()";

    if('-file' in myargvs):
        log_file = myargvs['-file'];
        
    if('-func' in myargvs):
        func = myargvs['-func']+'()';
    print(func)

    if('-count' in myargvs):
        count = (int)(myargvs['-count']);

    parse_trace(log_file, count, func)

if __name__ == "__main__":
    main();

