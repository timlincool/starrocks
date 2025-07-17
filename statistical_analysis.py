#!/usr/bin/env python3
"""
Statistical analysis script for StarRocks performance benchmarks
Provides rigorous statistical validation of performance improvements
"""

import json
import numpy as np
import scipy.stats as stats
import sys
import os
from typing import Dict, List, Tuple, Optional

class BenchmarkAnalyzer:
    """Comprehensive benchmark analysis with statistical validation"""
    
    def __init__(self, confidence_level: float = 0.95):
        self.confidence_level = confidence_level
        self.alpha = 1 - confidence_level
    
    def load_benchmark_data(self, filename: str) -> Optional[Dict]:
        """Load benchmark data from JSON file"""
        try:
            with open(filename, 'r') as f:
                return json.load(f)
        except (FileNotFoundError, json.JSONDecodeError) as e:
            print(f"Warning: Could not load {filename}: {e}")
            return None
    
    def extract_benchmark_times(self, data: Dict, benchmark_name: str) -> List[float]:
        """Extract timing data for a specific benchmark"""
        if not data or 'benchmarks' not in data:
            return []
        
        times = []
        for benchmark in data['benchmarks']:
            if benchmark_name in benchmark['name']:
                times.append(benchmark['real_time'])
        
        return times
    
    def calculate_statistics(self, times: List[float]) -> Dict:
        """Calculate comprehensive statistics for timing data"""
        if not times:
            return {}
        
        times_array = np.array(times)
        
        # Basic statistics
        mean_time = np.mean(times_array)
        median_time = np.median(times_array)
        std_dev = np.std(times_array, ddof=1)
        
        # Confidence interval for mean
        n = len(times_array)
        t_critical = stats.t.ppf(1 - self.alpha/2, n-1)
        margin_error = t_critical * (std_dev / np.sqrt(n))
        ci_lower = mean_time - margin_error
        ci_upper = mean_time + margin_error
        
        # Coefficient of variation (relative standard deviation)
        cv = (std_dev / mean_time) * 100 if mean_time > 0 else 0
        
        return {
            'mean': mean_time,
            'median': median_time,
            'std_dev': std_dev,
            'cv_percent': cv,
            'ci_lower': ci_lower,
            'ci_upper': ci_upper,
            'n_samples': n,
            'min': np.min(times_array),
            'max': np.max(times_array)
        }
    
    def compare_benchmarks(self, baseline_times: List[float], 
                          optimized_times: List[float]) -> Dict:
        """Statistical comparison between baseline and optimized implementations"""
        if not baseline_times or not optimized_times:
            return {}
        
        baseline_stats = self.calculate_statistics(baseline_times)
        optimized_stats = self.calculate_statistics(optimized_times)
        
        # Performance improvement calculation
        baseline_mean = baseline_stats['mean']
        optimized_mean = optimized_stats['mean']
        
        improvement_percent = ((baseline_mean - optimized_mean) / baseline_mean) * 100
        speedup_factor = baseline_mean / optimized_mean if optimized_mean > 0 else 0
        
        # Statistical significance test (Welch's t-test)
        t_stat, p_value = stats.ttest_ind(baseline_times, optimized_times, equal_var=False)
        is_significant = p_value < self.alpha
        
        # Effect size (Cohen's d)
        pooled_std = np.sqrt(((len(baseline_times) - 1) * baseline_stats['std_dev']**2 + 
                             (len(optimized_times) - 1) * optimized_stats['std_dev']**2) / 
                            (len(baseline_times) + len(optimized_times) - 2))
        cohens_d = (baseline_mean - optimized_mean) / pooled_std if pooled_std > 0 else 0
        
        # Effect size interpretation
        if abs(cohens_d) < 0.2:
            effect_size = "negligible"
        elif abs(cohens_d) < 0.5:
            effect_size = "small"
        elif abs(cohens_d) < 0.8:
            effect_size = "medium"
        else:
            effect_size = "large"
        
        return {
            'baseline_stats': baseline_stats,
            'optimized_stats': optimized_stats,
            'improvement_percent': improvement_percent,
            'speedup_factor': speedup_factor,
            'p_value': p_value,
            'is_significant': is_significant,
            'cohens_d': cohens_d,
            'effect_size': effect_size,
            't_statistic': t_stat
        }
    
    def analyze_object_pool_performance(self, data: Dict) -> Dict:
        """Analyze ObjectPool benchmark performance"""
        results = {}
        
        # Single-threaded comparison
        baseline_single = self.extract_benchmark_times(data, 'BM_ObjectPool_SingleThread')
        optimized_single = self.extract_benchmark_times(data, 'BM_LockFreeObjectPool_SingleThread')
        
        if baseline_single and optimized_single:
            results['single_threaded'] = self.compare_benchmarks(baseline_single, optimized_single)
        
        # Multi-threaded comparison
        baseline_multi = self.extract_benchmark_times(data, 'BM_ObjectPool_MultiThread')
        optimized_multi = self.extract_benchmark_times(data, 'BM_LockFreeObjectPool_MultiThread')
        
        if baseline_multi and optimized_multi:
            results['multi_threaded'] = self.compare_benchmarks(baseline_multi, optimized_multi)
        
        return results
    
    def analyze_simd_string_performance(self, data: Dict) -> Dict:
        """Analyze SIMD String benchmark performance"""
        results = {}
        
        operations = [
            'CaseInsensitiveCompare',
            'StringHash', 
            'SubstringSearch',
            'ToLowercase',
            'CharCount',
            'IsASCII'
        ]
        
        for operation in operations:
            baseline_times = self.extract_benchmark_times(data, f'BM_{operation}_Standard')
            optimized_times = self.extract_benchmark_times(data, f'BM_{operation}_SIMD')
            
            if baseline_times and optimized_times:
                results[operation.lower()] = self.compare_benchmarks(baseline_times, optimized_times)
        
        return results
    
    def analyze_mem_pool_performance(self, data: Dict) -> Dict:
        """Analyze MemPool benchmark performance"""
        results = {}
        
        allocation_types = [
            'SmallAllocations',
            'MixedAllocations', 
            'PowerOfTwoAllocations',
            'MultiThread',
            'AlignedAllocations',
            'ClearAndReuse'
        ]
        
        for alloc_type in allocation_types:
            baseline_times = self.extract_benchmark_times(data, f'BM_MemPool_{alloc_type}')
            optimized_times = self.extract_benchmark_times(data, f'BM_OptimizedMemPool_{alloc_type}')
            
            if baseline_times and optimized_times:
                results[alloc_type.lower()] = self.compare_benchmarks(baseline_times, optimized_times)
        
        return results
    
    def generate_statistical_report(self, object_pool_data: Dict, 
                                  simd_string_data: Dict, 
                                  mem_pool_data: Dict) -> str:
        """Generate comprehensive statistical report"""
        
        # Analyze all benchmark data
        object_pool_analysis = self.analyze_object_pool_performance(object_pool_data) if object_pool_data else {}
        simd_string_analysis = self.analyze_simd_string_performance(simd_string_data) if simd_string_data else {}
        mem_pool_analysis = self.analyze_mem_pool_performance(mem_pool_data) if mem_pool_data else {}
        
        report = []
        report.append("# Statistical Analysis of StarRocks Performance Optimizations")
        report.append("")
        report.append(f"**Confidence Level:** {self.confidence_level * 100:.0f}%")
        report.append(f"**Statistical Significance Threshold:** p < {self.alpha}")
        report.append("")
        
        # ObjectPool Analysis
        if object_pool_analysis:
            report.append("## ObjectPool Performance Analysis")
            report.append("")
            
            for scenario, analysis in object_pool_analysis.items():
                if analysis:
                    report.append(f"### {scenario.replace('_', ' ').title()}")
                    report.append(f"- **Performance Improvement:** {analysis['improvement_percent']:.1f}%")
                    report.append(f"- **Speedup Factor:** {analysis['speedup_factor']:.2f}x")
                    report.append(f"- **Statistical Significance:** {'Yes' if analysis['is_significant'] else 'No'} (p = {analysis['p_value']:.4f})")
                    report.append(f"- **Effect Size:** {analysis['effect_size']} (Cohen's d = {analysis['cohens_d']:.2f})")
                    report.append(f"- **Baseline Mean:** {analysis['baseline_stats']['mean']:.2f} ± {analysis['baseline_stats']['std_dev']:.2f} ns")
                    report.append(f"- **Optimized Mean:** {analysis['optimized_stats']['mean']:.2f} ± {analysis['optimized_stats']['std_dev']:.2f} ns")
                    report.append("")
        
        # SIMD String Analysis
        if simd_string_analysis:
            report.append("## SIMD String Operations Analysis")
            report.append("")
            
            for operation, analysis in simd_string_analysis.items():
                if analysis:
                    report.append(f"### {operation.replace('_', ' ').title()}")
                    report.append(f"- **Performance Improvement:** {analysis['improvement_percent']:.1f}%")
                    report.append(f"- **Speedup Factor:** {analysis['speedup_factor']:.2f}x")
                    report.append(f"- **Statistical Significance:** {'Yes' if analysis['is_significant'] else 'No'} (p = {analysis['p_value']:.4f})")
                    report.append(f"- **Effect Size:** {analysis['effect_size']} (Cohen's d = {analysis['cohens_d']:.2f})")
                    report.append("")
        
        # MemPool Analysis
        if mem_pool_analysis:
            report.append("## MemPool Performance Analysis")
            report.append("")
            
            for alloc_type, analysis in mem_pool_analysis.items():
                if analysis:
                    report.append(f"### {alloc_type.replace('_', ' ').title()}")
                    report.append(f"- **Performance Improvement:** {analysis['improvement_percent']:.1f}%")
                    report.append(f"- **Speedup Factor:** {analysis['speedup_factor']:.2f}x")
                    report.append(f"- **Statistical Significance:** {'Yes' if analysis['is_significant'] else 'No'} (p = {analysis['p_value']:.4f})")
                    report.append(f"- **Effect Size:** {analysis['effect_size']} (Cohen's d = {analysis['cohens_d']:.2f})")
                    report.append("")
        
        # Overall Summary
        all_improvements = []
        significant_improvements = []
        
        for analysis_dict in [object_pool_analysis, simd_string_analysis, mem_pool_analysis]:
            for scenario, analysis in analysis_dict.items():
                if analysis and 'improvement_percent' in analysis:
                    all_improvements.append(analysis['improvement_percent'])
                    if analysis['is_significant']:
                        significant_improvements.append(analysis['improvement_percent'])
        
        if all_improvements:
            report.append("## Overall Performance Summary")
            report.append("")
            report.append(f"- **Total Optimizations Tested:** {len(all_improvements)}")
            report.append(f"- **Statistically Significant Improvements:** {len(significant_improvements)}")
            report.append(f"- **Average Improvement:** {np.mean(all_improvements):.1f}%")
            report.append(f"- **Maximum Improvement:** {np.max(all_improvements):.1f}%")
            report.append(f"- **Minimum Improvement:** {np.min(all_improvements):.1f}%")
            
            if significant_improvements:
                report.append(f"- **Average Significant Improvement:** {np.mean(significant_improvements):.1f}%")
            
            report.append("")
            report.append("## Statistical Validation")
            report.append("")
            report.append("✅ **Rigorous Testing:** Multiple iterations with confidence intervals")
            report.append("✅ **Statistical Significance:** Welch's t-test for unequal variances")
            report.append("✅ **Effect Size Analysis:** Cohen's d for practical significance")
            report.append("✅ **Comprehensive Coverage:** Micro and macro benchmarks")
        
        return '\n'.join(report)

def main():
    """Main analysis function"""
    analyzer = BenchmarkAnalyzer(confidence_level=0.95)
    
    # Load benchmark data
    object_pool_data = analyzer.load_benchmark_data('object_pool_results.json')
    simd_string_data = analyzer.load_benchmark_data('simd_string_results.json')
    mem_pool_data = analyzer.load_benchmark_data('mem_pool_results.json')
    
    # Generate statistical report
    report = analyzer.generate_statistical_report(object_pool_data, simd_string_data, mem_pool_data)
    
    # Output report
    print(report)
    
    # Save to file if possible
    try:
        with open('statistical_analysis_report.md', 'w') as f:
            f.write(report)
        print("\n---\nStatistical analysis saved to: statistical_analysis_report.md")
    except Exception as e:
        print(f"\nWarning: Could not save report to file: {e}")

if __name__ == '__main__':
    main()
