import argparse
import os

parser = argparse.ArgumentParser()
parser.add_argument('-f', dest='file')

def parse_output(file):
    ret = dict()
    i = 0
    cycle = []
    for line in file.readlines():
        if line == '\n':
            ret[i] = cycle
            i += 1
            cycle = []
        else:
            cycle.append(line)
    return ret
    

def print_difference(student, ref):
    s = parse_output(student)
    r = parse_output(ref)
    for i in range(len(r)):
        for s_i, r_i in zip(s[i], r[i]):
            if s_i != r_i:
                print(f'First difference from reference detected in cycle {i}')
                print(f'Your output: {s_i}')
                print(f'Reference output: {r_i}')
                return False
    print('All cycles matched!')
    return True


if __name__ == "__main__":
    args = parser.parse_args()

    if args.file:
        os.system(f'./psim {args.file} > student.txt')
        os.system(f'./psim-ref {args.file} > ref.txt')
        with open('student.txt', 'r') as f:
            with open('ref.txt', 'r') as g:
                result = print_difference(f,g)
        if result:
            os.system('rm student.txt')
            os.system('ref.txt')
    else:
        print('Please provide a test .yo file.')